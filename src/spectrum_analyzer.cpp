// ============================================================================
// spectrum_analyzer.cpp
//  Low-priority FFT analysis and bass-to-treble spectrum levels
// ============================================================================
//
// WHAT THIS FILE DOES
// -------------------
// A WAV player receives speaker samples, not named musical notes. A low bass
// hit and a bright hi-hat are mixed together in the same stream of numbers.
// To make visualizer motion follow the real song, this file takes small
// snapshots of those PCM samples and uses an FFT (Fast Fourier Transform) to
// ask: "how much energy is present at low, middle, and high frequencies?"
//
// This is deliberately different from the older nRF52840 MIDI visualizer:
// that project already knew which MIDI key was pressed. Its conversion tool
// mapped low note numbers into bass-side bars and high note numbers into
// treble-side bars, then saved bar animation frames in firmware. Here there
// are no MIDI note messages, so we measure the actual WAV sound in real time.
//
// PERFORMANCE MAP
// ---------------
//   audio_player.cpp finishes feeding an I2S DMA chunk
//       -> spectrum_analyzer_submit_pcm() occasionally copies 1024 samples
//       -> one-slot FreeRTOS queue keeps only the newest useful snapshot
//       -> low-priority spectrum_task() performs FFT work
//       -> spectrum_analyzer_get_levels() returns 13 cheap 0..100 bar values
//
// The important rule is that FFT work never runs before DMA has received its
// sound data. Buttons and playback timing remain more important than visuals.

#include <Arduino.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "spectrum_analyzer.h"

// 1024 samples gives useful bass separation without becoming a large CPU/RAM
// job. At 44.1 kHz, each FFT bin represents about 43 Hz and each snapshot
// covers about 23 ms of sound.
static constexpr size_t FFT_SAMPLE_COUNT = 1024;

// Publish a fresh spectrum roughly twenty-two times a second. The one-frame
// queue and low-priority worker still protect audio playback, while the UI
// receives enough new contour points for a fluid 30 fps interpolation.
static constexpr uint32_t FFT_CAPTURE_INTERVAL_MS = 45;

// Anything quieter than this is treated as rest. Turning decibels into a
// 0..100 output makes the visualizer independent from FFT mathematics.
static constexpr float FFT_NOISE_FLOOR_DB = -60.0f;

struct SpectrumFrame
{
    // The same FFT bin means a different frequency at different WAV sample
    // rates, so the worker must receive the current song's sample rate.
    uint32_t sample_rate;

    // Reset increments this stamp. A worker finishing old-song or pre-pause
    // work sees a stale generation and quietly throws the result away.
    uint32_t generation;

    // Analysis is mono because a display bar needs overall song energy, not
    // left/right imaging. Stereo is mixed down while the snapshot is copied.
    int16_t samples[FFT_SAMPLE_COUNT];
};

// Analysis owns its own task and queue. The audio engine knows only the tiny
// public API in spectrum_analyzer.h and never waits for the FFT to finish.
static TaskHandle_t spectrum_task_handle = nullptr;
static QueueHandle_t spectrum_queue = nullptr;

// This is the published answer: index 0 is deep bass, the final index is high
// treble. Reading 13 bytes later is far cheaper than touching FFT buffers.
static volatile uint8_t spectrum_levels[SPECTRUM_BAND_COUNT] = {};

// Generation and capture timing let start/pause/track-change invalidate stale
// animation data and keep analysis throttled independently from audio output.
static volatile uint32_t spectrum_generation = 0;
static uint32_t last_spectrum_capture_ms = 0;

static void clear_spectrum(void)
{
    // Reset the display source immediately. Once a visualizer consumes this
    // API, pause should collapse the shape instead of leaving frozen peaks.
    for (size_t i = 0; i < SPECTRUM_BAND_COUNT; ++i)
    {
        spectrum_levels[i] = 0;
    }
}

void spectrum_analyzer_reset(void)
{
    // It is not enough to zero visible bars: an FFT could already be queued or
    // running. The generation stamp makes that in-flight answer harmless.
    spectrum_generation++;
    last_spectrum_capture_ms = 0;
    clear_spectrum();
    if (spectrum_queue)
    {
        xQueueReset(spectrum_queue);
    }
}

static void fft_in_place(float data[FFT_SAMPLE_COUNT * 2])
{
    // Complex FFT values are stored interleaved:
    //   data[2 * i]     = real part of sample/bin i
    //   data[2 * i + 1] = imaginary part of sample/bin i
    // Input sound starts with an imaginary part of zero; output becomes the
    // complex amplitude for each frequency bin.

    // A radix-2 FFT repeatedly rotates values by predictable complex angles.
    // Calculate the one base rotation needed per stage only on the first FFT,
    // instead of running sin/cos throughout every song.
    static float twiddle_real[10];
    static float twiddle_imag[10];
    static bool twiddles_ready = false;
    if (!twiddles_ready)
    {
        for (size_t length = 2, stage = 0; length <= FFT_SAMPLE_COUNT; length <<= 1, ++stage)
        {
            float angle = (-2.0f * PI) / (float)length;
            twiddle_real[stage] = cosf(angle);
            twiddle_imag[stage] = sinf(angle);
        }
        twiddles_ready = true;
    }

    // Cooley-Tukey FFT butterflies operate correctly after the array is put in
    // bit-reversed order. For 1024 samples that is a cheap in-place shuffle,
    // avoiding a second large work buffer.
    for (size_t i = 1, j = 0; i < FFT_SAMPLE_COUNT; ++i)
    {
        size_t bit = FFT_SAMPLE_COUNT >> 1;
        for (; j & bit; bit >>= 1)
        {
            j ^= bit;
        }
        j ^= bit;
        if (i < j)
        {
            float swap = data[i * 2];
            data[i * 2] = data[j * 2];
            data[j * 2] = swap;
            swap = data[(i * 2) + 1];
            data[(i * 2) + 1] = data[(j * 2) + 1];
            data[(j * 2) + 1] = swap;
        }
    }

    // Combine pairs, then groups of four, eight, and so on until every sample
    // contributes to every output bin. The rotation update is multiply/add
    // math rather than repeated trigonometry in the hot inner loop.
    for (size_t length = 2, stage = 0; length <= FFT_SAMPLE_COUNT; length <<= 1, ++stage)
    {
        for (size_t start = 0; start < FFT_SAMPLE_COUNT; start += length)
        {
            float rotation_real = 1.0f;
            float rotation_imag = 0.0f;
            for (size_t offset = 0; offset < length / 2; ++offset)
            {
                size_t even = (start + offset) * 2;
                size_t odd = (start + offset + (length / 2)) * 2;
                float odd_real = (rotation_real * data[odd]) - (rotation_imag * data[odd + 1]);
                float odd_imag = (rotation_real * data[odd + 1]) + (rotation_imag * data[odd]);
                float next_real = (rotation_real * twiddle_real[stage]) -
                                  (rotation_imag * twiddle_imag[stage]);

                data[odd] = data[even] - odd_real;
                data[odd + 1] = data[even + 1] - odd_imag;
                data[even] += odd_real;
                data[even + 1] += odd_imag;
                rotation_imag = (rotation_real * twiddle_imag[stage]) +
                                (rotation_imag * twiddle_real[stage]);
                rotation_real = next_real;
            }
        }
    }
}

static void analyse_spectrum(const SpectrumFrame &frame)
{
    // These buffers are static so the task does not put several kilobytes of
    // temporary FFT memory on its stack every time a frame arrives.
    static float fft_data[FFT_SAMPLE_COUNT * 2];
    static float window[FFT_SAMPLE_COUNT];
    static bool window_ready = false;
    if (!window_ready)
    {
        // A raw rectangular sample cut creates false spectral splatter where
        // the beginning and end do not line up. A Hann window gently fades the
        // edges and gives cleaner bass/treble bars. It never changes, so cache it.
        for (size_t i = 0; i < FFT_SAMPLE_COUNT; ++i)
        {
            window[i] = 0.5f - (0.5f * cosf((2.0f * PI * i) / (FFT_SAMPLE_COUNT - 1)));
        }
        window_ready = true;
    }

    if (frame.generation != spectrum_generation)
    {
        // A pause or song switch happened after this snapshot was captured.
        // Do not revive animation from sound that is no longer current.
        return;
    }

    // Remove the average sample offset (the DC component). Otherwise a small
    // electrical/file offset can inflate the lowest frequency band.
    float mean = 0.0f;
    for (size_t i = 0; i < FFT_SAMPLE_COUNT; ++i)
    {
        mean += (float)frame.samples[i];
    }
    mean /= (float)FFT_SAMPLE_COUNT;

    // Prepare real PCM samples as windowed complex FFT input.
    for (size_t i = 0; i < FFT_SAMPLE_COUNT; ++i)
    {
        fft_data[i * 2] = ((float)frame.samples[i] - mean) * window[i];
        fft_data[(i * 2) + 1] = 0.0f;
    }

    fft_in_place(fft_data);

    // Human-friendly bands are spaced roughly logarithmically: bass gets
    // tighter ranges where small frequency differences feel significant, and
    // treble receives wider ranges. Index 0 begins at 20 Hz; index 12 reaches
    // toward 16 kHz. A visualizer can draw them left-to-right as bass->treble.
    static const uint16_t band_edges[SPECTRUM_BAND_COUNT + 1] = {
        20, 60, 110, 180, 280, 430, 650, 1000, 1500, 2300, 3500, 5500, 8500, 16000};

    // FFT results are array bins, while band_edges are in Hz. bin_width
    // translates between them for any accepted WAV sample rate.
    const float bin_width = (float)frame.sample_rate / (float)FFT_SAMPLE_COUNT;
    const size_t last_bin = FFT_SAMPLE_COUNT / 2;

    // PCM is signed 16-bit. Normalizing against an approximate full-scale
    // windowed signal gives useful, comparable decibel values across songs.
    const float full_scale_power =
        (FFT_SAMPLE_COUNT * 32768.0f / 4.0f) * (FFT_SAMPLE_COUNT * 32768.0f / 4.0f);
    uint8_t next_levels[SPECTRUM_BAND_COUNT] = {};

    for (size_t band = 0; band < SPECTRUM_BAND_COUNT; ++band)
    {
        // Ignore bin 0 because it is DC, not audible bass. Clamp the high end
        // at Nyquist: sampled audio cannot contain meaningful bins above half
        // the WAV sample rate.
        size_t first = max((size_t)1, (size_t)ceilf(band_edges[band] / bin_width));
        size_t end = min(last_bin, (size_t)ceilf(band_edges[band + 1] / bin_width));
        float strongest_power = 0.0f;

        // The strongest frequency in a band creates crisp musical response:
        // one clear bass note should lift its bar without being averaged away.
        // Power uses real^2 + imag^2, deliberately avoiding an expensive sqrt.
        for (size_t bin = first; bin < end; ++bin)
        {
            float real = fft_data[bin * 2];
            float imag = fft_data[(bin * 2) + 1];
            float power = (real * real) + (imag * imag);
            strongest_power = max(strongest_power, power);
        }

        // Perceived loudness behaves closer to decibels than linear amplitude.
        // Map -60 dB..0 dB to 0..100, then retain only a brief peak hold.
        // The UI also smooths bar positions; a long analyzer-side decay would
        // stack with that easing and make the large waveform look sluggish.
        float normalized_power = strongest_power / full_scale_power;
        float db = normalized_power > 0.000000000001f
                       ? 10.0f * log10f(normalized_power)
                       : FFT_NOISE_FLOOR_DB;
        uint8_t level = (uint8_t)constrain((int)((db - FFT_NOISE_FLOOR_DB) * (100.0f / -FFT_NOISE_FLOOR_DB)), 0, 100);
        uint8_t previous = spectrum_levels[band];
        next_levels[band] = level > previous ? level : (uint8_t)max(0, (int)previous - 18);
    }

    if (frame.generation == spectrum_generation)
    {
        // Reset may have occurred during FFT work, so check again before
        // publishing. This is what makes pause reliably settle at zero.
        for (size_t band = 0; band < SPECTRUM_BAND_COUNT; ++band)
        {
            spectrum_levels[band] = next_levels[band];
        }
    }
}

static void spectrum_task(void *unused)
{
    (void)unused;

    // Only this worker performs FFT work. It blocks with no CPU usage until
    // the audio side deposits a snapshot, then returns to waiting.
    static SpectrumFrame frame;
    for (;;)
    {
        if (xQueueReceive(spectrum_queue, &frame, portMAX_DELAY) == pdTRUE)
        {
            analyse_spectrum(frame);
        }
    }
}

bool spectrum_analyzer_begin(void)
{
    // Starting a new track reuses the existing task. There should never be two
    // FFT workers competing for CPU or publishing conflicting bar values.
    if (spectrum_task_handle)
    {
        return true;
    }

    if (!spectrum_queue)
    {
        // Length one is intentional: for animation, the newest picture of the
        // music matters; processing a backlog would only make motion late.
        spectrum_queue = xQueueCreate(1, sizeof(SpectrumFrame));
    }
    if (!spectrum_queue)
    {
        return false;
    }

    // FFT math stays below DMA feeding. One slot discards stale visual frames
    // instead of accumulating work the display will never need.
    BaseType_t task_started = xTaskCreatePinnedToCore(
        spectrum_task, "spectrum", 4096, nullptr, 1, &spectrum_task_handle, 0);
    if (task_started != pdPASS)
    {
        vQueueDelete(spectrum_queue);
        spectrum_queue = nullptr;
        return false;
    }
    return true;
}

void spectrum_analyzer_submit_pcm(const uint8_t *pcm_bytes,
                                  size_t byte_count,
                                  uint16_t channels,
                                  uint32_t sample_rate)
{
    // audio_player calls this after i2s_write(), when the important playback
    // work is already secured in DMA. Capturing the generation first ties this
    // snapshot to the playback state in which it was heard.
    uint32_t generation = spectrum_generation;
    if (!spectrum_queue || channels == 0 ||
        millis() - last_spectrum_capture_ms < FFT_CAPTURE_INTERVAL_MS)
    {
        // Skip work if analysis is unavailable, malformed, or too soon after
        // the last frame. None of these cases should ever interrupt playback.
        return;
    }

    size_t frame_count = byte_count / (sizeof(int16_t) * channels);
    if (frame_count < FFT_SAMPLE_COUNT)
    {
        // The FFT needs one complete 1024-sample window. A short final read at
        // the end of a track is not padded, because silence padding would only
        // create a misleading last animation frame.
        return;
    }

    last_spectrum_capture_ms = millis();
    static SpectrumFrame capture;
    capture.sample_rate = sample_rate;
    capture.generation = generation;
    const int16_t *samples = (const int16_t *)pcm_bytes;
    for (size_t i = 0; i < FFT_SAMPLE_COUNT; ++i)
    {
        // WAV mono already represents total energy. WAV stereo is averaged to
        // one visual signal so the display responds to both speakers without
        // doubling FFT cost.
        int32_t mono = samples[i * channels];
        if (channels == 2)
        {
            mono = (mono + samples[(i * channels) + 1]) / 2;
        }
        capture.samples[i] = (int16_t)mono;
    }

    // If a prior analysis frame is waiting, overwrite it. Fresh motion is more
    // useful than exact history for a visualizer, and playback never blocks.
    xQueueOverwrite(spectrum_queue, &capture);
}

void spectrum_analyzer_get_levels(uint8_t levels[SPECTRUM_BAND_COUNT])
{
    // The UI copies only the final small result. It does not touch the queue,
    // sample snapshots, or FFT workspace, keeping drawing lightweight.
    for (size_t i = 0; i < SPECTRUM_BAND_COUNT; ++i)
    {
        levels[i] = spectrum_levels[i];
    }
}
