// ============================================================================
// audio_player.cpp
//  WAV/MP3 playback, pause, next, volume
// ============================================================================
//
// AUDIO THEORY MAP
// ----------------
// A PCM WAV file is already a timeline of speaker sample numbers; there is no
// MP3-style decompression step here. For 16-bit stereo it looks conceptually
// like: left sample, right sample, left sample, right sample, over and over.
//
// Flow through this module:
//   SD_MMC File -> stream_audio_chunk() -> I2S DMA buffers -> ES8311 codec
//   -> speaker/headphone output.
//
// I2S is a digital audio wire protocol. It sends:
//   MCLK = master clock used by the codec internally
//   BCLK = ticks individual sample bits across the wire
//   LRCK = identifies left vs right sample timing
//   SDOUT = the actual PCM bit stream
//
// DMA (direct memory access) is the reason playback can be smooth: hardware
// drains queued sample buffers into I2S without asking the CPU for every bit.
// But DMA is not infinite. If UI redraws block the only code feeding it, its
// queue empties and the speaker produces clicks/broken sound. This project
// fixed that by feeding DMA from audio_stream_task() independently of LVGL.

#include <SD_MMC.h>
#include "driver/i2s.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "../_official_3p5_demo/Arduino/libraries/es8311/es8311.h"
#include "../_official_3p5_demo/Arduino/libraries/es8311/es8311.cpp"
#include "audio_player.h"
#include "spectrum_analyzer.h"

#define I2S_MCLK 12
#define I2S_BCLK 13
#define I2S_LRCK 15
#define I2S_SDOUT 16

#define AUDIO_MCLK_MULTIPLE 256
#define AUDIO_START_VOLUME 70

// DMA TUNING MAP
// --------------
// More queued frames make audio harder to interrupt, but also mean more sound
// is already "in flight" when pause is pressed and when a visualizer responds.
// At 44.1 kHz, one 512-frame block lasts about 11.6 ms; twelve blocks can hold
// about 139 ms. Keep these names visible for hardware testing: if touch/audio
// is stable, AUDIO_DMA_BUFFER_COUNT is the first latency knob to try at 8,
// then 6, listening carefully for clicks during swipes and drawer movement.
static constexpr int AUDIO_DMA_BUFFER_COUNT = 12;
static constexpr int AUDIO_DMA_FRAMES_PER_BUFFER = 512;

// How this module links to the screen:
// music_controller_start() calls audio_player_start_wav().
// A private FreeRTOS task below keeps audio streaming while LVGL draws.
// music_controller_update() only reads last_error for screen messages.
// pause_button_event_cb() in display_ui.cpp calls audio_player_toggle_pause().

struct WavInfo
{
    // Stuff we pull out of the WAV header before audio starts.
    // The actual sound bytes live later in the file, after the header chunks.
    uint32_t sample_rate = 0;
    uint32_t data_offset = 0;
    uint32_t data_size = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
};

// `wav_file` stays open for the current song. It is read only by the audio
// task while audio_mutex is held; next/previous takes that same mutex before
// closing/replacing it. That prevents "read from a file while another context
// closes it" crashes and corrupted transitions.
static File wav_file;
static WavInfo current_wav;
// These values are shared between two jobs:
// audio_stream_task() reads the file on its own core, while the LVGL/button
// side toggles pause and reads its status. Volatile makes each job re-check
// the tiny flags instead of keeping an old value while the other job changes it.
static volatile bool audio_playing = false;
static volatile bool audio_paused = false;
static volatile uint32_t bytes_played = 0;
static const char *volatile last_error = "";
static TaskHandle_t audio_task_handle = nullptr;
static SemaphoreHandle_t audio_mutex = nullptr;
// ES8311 is the physical digital-to-analog codec. Saving its handle lets the
// volume slider change output gain immediately without restarting playback.
static es8311_handle_t codec_handle = nullptr;
static bool i2s_ready = false;
static uint8_t playback_volume = AUDIO_START_VOLUME;

static void stream_audio_chunk(void);

static void audio_stream_task(void *unused)
{
    (void)unused;

    // This task is deliberately separate from loop() in main.cpp. A menu
    // animation can spend time pushing pixels to the LCD; this job still gets
    // scheduled and feeds I2S before its DMA queue goes empty and sounds broken.
    for (;;)
    {
        if (audio_playing && !audio_paused)
        {
            // stream_audio_chunk() normally blocks inside i2s_write() until
            // DMA has room. That is useful back-pressure: we naturally read
            // SD at approximately the rate the speaker consumes samples.
            stream_audio_chunk();
        }
        else
        {
            // Paused/stopped music does not need to spin at full speed.
            // The next linked action is audio_player_toggle_pause(), which
            // flips audio_paused and lets this task start feeding samples again.
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

static uint16_t read_u16(File &file)
{
    // WAV files store numbers little-endian, meaning the smallest byte comes first.
    // So two bytes like [0x44, 0xAC] become 0xAC44.
    uint8_t b[2];
    if (file.read(b, sizeof(b)) != sizeof(b))
    {
        return 0;
    }
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

static uint32_t read_u32(File &file)
{
    // Same little-endian idea as read_u16(), but for four-byte numbers.
    // WAV chunk sizes and sample rates use this a lot.
    uint8_t b[4];
    if (file.read(b, sizeof(b)) != sizeof(b))
    {
        return 0;
    }
    return (uint32_t)b[0] |
           ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

static bool read_fourcc(File &file, char out[5])
{
    // FourCC means "four character code".
    // WAV chunks are tagged with names like "RIFF", "WAVE", "fmt ", and "data".
    if (file.read((uint8_t *)out, 4) != 4)
    {
        return false;
    }
    out[4] = '\0';
    return true;
}

static bool fourcc_equals(const char *a, const char *b)
{
    // Only compare the first 4 chars because FourCC tags are exactly 4 bytes.
    // "fmt " has a space at the end on purpose.
    return strncmp(a, b, 4) == 0;
}

static bool parse_wav_header(File &file, WavInfo &info)
{
    // A normal WAV starts with RIFF, then a file size, then WAVE.
    // If this fails, it probably is not a WAV at all.
    char id[5];
    if (!read_fourcc(file, id) || !fourcc_equals(id, "RIFF"))
    {
        return false;
    }

    // The RIFF total-size field is useful for generic parsers, but playback
    // cares about the later `data` chunk size, so we only step past it here.
    read_u32(file);
    if (!read_fourcc(file, id) || !fourcc_equals(id, "WAVE"))
    {
        return false;
    }

    bool found_fmt = false;
    bool found_data = false;
    uint16_t audio_format = 0;

    // WAV files are made of chunks. We keep hopping chunk to chunk until we find:
    // "fmt "  = what kind of audio this is
    // "data" = where the actual speaker samples begin
    while (file.available())
    {
        if (!read_fourcc(file, id))
        {
            break;
        }

        uint32_t chunk_size = read_u32(file);
        // WAV chunks are padded to an even byte boundary. If a metadata chunk
        // has an odd length, the `+ (chunk_size & 1)` skips its pad byte.
        uint32_t next_chunk = file.position() + chunk_size + (chunk_size & 1);

        if (fourcc_equals(id, "fmt "))
        {
            // PCM format is the simple raw-audio kind we can stream directly.
            // Compressed WAV exists too, but that needs a decoder, so we reject it below.
            audio_format = read_u16(file);
            info.channels = read_u16(file);
            info.sample_rate = read_u32(file);
            // Byte rate and frame/block alignment are in the fmt chunk too,
            // but this simple PCM streamer does not need them after validating
            // channels, sample rate, and bit depth.
            read_u32(file);
            read_u16(file);
            info.bits_per_sample = read_u16(file);
            found_fmt = true;
        }
        else if (fourcc_equals(id, "data"))
        {
            // This is the treasure chest: raw PCM sample bytes.
            // Save where it starts and how many bytes it has, then playback can begin.
            info.data_offset = file.position();
            info.data_size = chunk_size;
            found_data = true;
            break;
        }

        file.seek(next_chunk);
    }

    // Keep the first version intentionally simple:
    // format 1 = uncompressed PCM, 16-bit samples, mono or stereo, reasonable sample rate.
    return found_fmt &&
           found_data &&
           audio_format == 1 &&
           info.channels >= 1 &&
           info.channels <= 2 &&
           info.bits_per_sample == 16 &&
           info.sample_rate >= 8000 &&
           info.sample_rate <= 96000;
}

static bool init_audio_codec(uint32_t sample_rate)
{
    // ES8311 is the little audio codec chip that turns I2S digital audio into speaker/headphone sound.
    // We set it to the same sample rate as the WAV so the song does not play too fast or too slow.
    if (!codec_handle)
    {
        codec_handle = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
    }
    if (!codec_handle)
    {
        return false;
    }

    const es8311_clock_config_t es_clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = (int)(sample_rate * AUDIO_MCLK_MULTIPLE),
        .sample_frequency = (int)sample_rate};

    // Codec and I2S must agree on sample rate. Playing 44.1 kHz bytes through
    // clocks configured for 22.05 kHz would make audio run slow and pitched down.
    if (es8311_init(codec_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK)
    {
        return false;
    }

    // playback_volume can already have been changed by the UI slider. When
    // next/previous opens another WAV, keep the listener's chosen volume.
    es8311_voice_volume_set(codec_handle, playback_volume, NULL);
    es8311_microphone_config(codec_handle, false);

    // I2S is the audio "conveyor belt" from the ESP32 to the ES8311.
    // DMA buffers let hardware keep pushing sound while our task reads the next
    // SD chunk. These named tuning constants keep the sound-versus-latency
    // tradeoff measurable rather than burying it as magic numbers here.
    const i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = sample_rate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        // Twelve blocks x 512 sample frames is deliberate cushioning. The LCD can
        // have bursts of expensive SPI drawing during gestures; these queued
        // samples keep leaving the speaker smoothly during that burst.
        .dma_buf_count = AUDIO_DMA_BUFFER_COUNT,
        .dma_buf_len = AUDIO_DMA_FRAMES_PER_BUFFER,
        // APLL gives the audio clock a steadier frequency. Auto-clear emits
        // silence instead of replaying stale garbage if DMA ever under-runs.
        .use_apll = true,
        .tx_desc_auto_clear = true,
        .fixed_mclk = (int)(sample_rate * AUDIO_MCLK_MULTIPLE),
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        .bits_per_chan = I2S_BITS_PER_CHAN_16BIT};

    // These pins are the physical audio wires on the Waveshare board.
    // MCLK/BCLK/LRCK are clocks, SDOUT is the actual audio data.
    const i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_MCLK,
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_LRCK,
        .data_out_num = I2S_SDOUT,
        .data_in_num = I2S_PIN_NO_CHANGE};

    // Changing tracks may also mean changing WAV sample rate. The controller
    // has stopped the streaming task and holds audio_mutex before we get here,
    // so it is safe to reset I2S and configure it for the new song.
    if (i2s_ready)
    {
        // Do not leave the former song's queued samples around when selecting
        // a WAV with potentially different timing information.
        i2s_driver_uninstall(I2S_NUM_0);
        i2s_ready = false;
    }

    if (i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL) != ESP_OK)
    {
        return false;
    }

    if (i2s_set_pin(I2S_NUM_0, &pin_config) != ESP_OK)
    {
        return false;
    }

    i2s_zero_dma_buffer(I2S_NUM_0);
    i2s_ready = true;
    return true;
}

bool audio_player_start_wav(const String &path)
{
    // Previous/next can call this while the private playback task exists.
    // Pause new reads first, then use the mutex so an SD read cannot overlap
    // closing the old file and opening the new one.
    if (!audio_mutex)
    {
        audio_mutex = xSemaphoreCreateMutex();
        if (!audio_mutex)
        {
            last_error = "Audio lock failed";
            return false;
        }
    }

    audio_playing = false;
    // A mutex is like the single key to the audio-file workbench. The
    // streaming task and track-change code cannot both touch wav_file/I2S
    // while only one of them is holding the key.
    xSemaphoreTake(audio_mutex, portMAX_DELAY);
    if (i2s_ready)
    {
        // Immediately remove a tail of old-song samples so next/previous does
        // not sound as if a fraction of the last song leaks into the new one.
        i2s_zero_dma_buffer(I2S_NUM_0);
    }
    if (wav_file)
    {
        wav_file.close();
    }

    // Open the chosen file, read its WAV header, then configure audio hardware to match it.
    wav_file = SD_MMC.open(path, "r");
    if (!wav_file || !parse_wav_header(wav_file, current_wav))
    {
        last_error = "Use 16-bit PCM WAV";
        xSemaphoreGive(audio_mutex);
        return false;
    }

    if (!init_audio_codec(current_wav.sample_rate))
    {
        last_error = "Audio init failed";
        wav_file.close();
        xSemaphoreGive(audio_mutex);
        return false;
    }

    // Jump past the WAV header/chunks so the next read starts on real audio sample bytes.
    wav_file.seek(current_wav.data_offset);
    bytes_played = 0;
    spectrum_analyzer_reset();

    if (!audio_task_handle)
    {
        // Arduino loop()/LVGL runs on core 1 for this board. Keep SD-to-I2S
        // streaming on core 0 so display flushing cannot pause the song.
        // The task calls stream_audio_chunk() below; the UI only observes status.
        BaseType_t task_started = xTaskCreatePinnedToCore(
            // Priority 3 gives the feeding job reliable scheduling without
            // turning the entire UI into a lower-priority afterthought.
            audio_stream_task, "wav_stream", 4096, nullptr, 3, &audio_task_handle, 0);
        if (task_started != pdPASS)
        {
            wav_file.close();
            last_error = "Audio task failed";
            xSemaphoreGive(audio_mutex);
            return false;
        }
    }

    // The analyzer is optional display data; audio still starts if its worker
    // cannot be created. It remains lower priority than this DMA-feeding task.
    spectrum_analyzer_begin();

    audio_playing = true;
    audio_paused = false;
    last_error = "";
    xSemaphoreGive(audio_mutex);
    return true;
}

static void stop_audio_output(const char *screen_status)
{
    // Every way playback stops should come through here. Most importantly,
    // an SD removal must empty I2S DMA instead of letting a stale fragment
    // keep reaching the ES8311 speaker as a tick/click.
    audio_playing = false;
    audio_paused = false;
    if (i2s_ready)
    {
        // This line fixed the ticking after pulling the SD card: without it,
        // the last tiny DMA fragment could remain audible as repeated clicking.
        i2s_zero_dma_buffer(I2S_NUM_0);
    }
    if (wav_file)
    {
        wav_file.close();
    }
    spectrum_analyzer_reset();

    // Next linked step: music_controller_update() reads this message and
    // forwards it to display_ui_set_status() on the touchscreen.
    last_error = screen_status;
}

static void stream_audio_chunk(void)
{
    // This is called by audio_stream_task(), not by the touchscreen/display
    // loop. The larger chunk also cuts down how often streaming has to visit SD.
    // file_buffer gets raw bytes from the SD card.
    // stereo_buffer is only used when a mono WAV needs to be duplicated into left+right.
    static uint8_t file_buffer[4096];
    static int16_t stereo_buffer[4096];

    // Pausing is delightfully simple here: leave the file position alone,
    // and stop reading/sending chunks until the button says go again.
    if (!audio_mutex || xSemaphoreTake(audio_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    if (!audio_playing || audio_paused)
    {
        xSemaphoreGive(audio_mutex);
        return;
    }

    uint32_t remaining = current_wav.data_size - bytes_played;
    if (remaining == 0)
    {
        // No more sound bytes. Stop cleanly and clear the DMA buffer so no old audio hangs around.
        stop_audio_output("Finished");
        xSemaphoreGive(audio_mutex);
        return;
    }

    if (!wav_file || !wav_file.available())
    {
        // We expected more WAV bytes, but the file disappeared early. That is
        // exactly what happens when the SD card is pulled during the song.
        stop_audio_output("SD removed");
        xSemaphoreGive(audio_mutex);
        return;
    }

    size_t to_read = min((uint32_t)sizeof(file_buffer), remaining);
    // Reading blocks rather than individual samples is important: SD cards
    // have transaction overhead, so a chunky sequential read is far cheaper.
    size_t bytes_read = wav_file.read(file_buffer, to_read);
    if (bytes_read == 0)
    {
        // A card can disappear between available() above and this actual read.
        // Silence the output through the same emergency stop path.
        stop_audio_output("SD removed");
        xSemaphoreGive(audio_mutex);
        return;
    }

    bytes_played += bytes_read;

    // Stereo WAV data is already left/right/left/right, so we can send it as-is.
    const void *write_buffer = file_buffer;
    size_t write_bytes = bytes_read;
    uint16_t analysis_channels = current_wav.channels;
    uint32_t analysis_sample_rate = current_wav.sample_rate;

    if (current_wav.channels == 1)
    {
        // Mono WAV is one sample at a time.
        // The I2S setup expects stereo, so duplicate each mono sample into left and right.
        size_t sample_count = bytes_read / sizeof(int16_t);
        int16_t *mono_samples = (int16_t *)file_buffer;
        for (int i = (int)sample_count - 1; i >= 0; --i)
        {
            stereo_buffer[i * 2] = mono_samples[i];
            stereo_buffer[i * 2 + 1] = mono_samples[i];
        }
        // The destination buffer is twice as wide because it holds a separate
        // left and right copy for every original mono sample.
        write_buffer = stereo_buffer;
        write_bytes = sample_count * sizeof(int16_t) * 2;
    }

    // This is the actual "make noise" call: hand sample bytes to I2S DMA and
    // wait until it accepts them. Hardware plays queued samples afterward;
    // this function is stocking a shelf, not bit-banging the speaker itself.
    size_t bytes_written = 0;
    i2s_write(I2S_NUM_0, write_buffer, write_bytes, &bytes_written, portMAX_DELAY);
    xSemaphoreGive(audio_mutex);

    // Only after DMA accepts this sound block do we copy an occasional mono
    // snapshot for lower-priority FFT work. UI/analysis never delays stocking
    // the audio buffers that prevent underruns.
    spectrum_analyzer_submit_pcm(file_buffer, bytes_read, analysis_channels,
                                 analysis_sample_rate);
}

bool audio_player_toggle_pause(void)
{
    // Do not pretend a pause worked before a song starts or after it has ended.
    if (!audio_playing)
    {
        return false;
    }

    audio_paused = !audio_paused;
    if (audio_paused)
    {
        spectrum_analyzer_reset();
    }
    // A tiny chunk may already be on its way to the speaker when pause is tapped.
    // Let it finish instead of wiping queued samples and accidentally skipping audio.
    return true;
}

bool audio_player_is_paused(void)
{
    return audio_paused;
}

void audio_player_set_volume(uint8_t volume)
{
    playback_volume = min((uint8_t)100, volume);

    // The slider can be touched before a WAV has opened. Save the value either
    // way, and only talk to the codec once init_audio_codec() has created it.
    // Next song startup above reapplies the same saved level.
    if (codec_handle)
    {
        // Changing codec gain is cheap and does not rewrite or re-scale all
        // PCM samples in RAM. It is the embedded-audio equivalent of turning
        // the amp level knob while the stream continues.
        es8311_voice_volume_set(codec_handle, playback_volume, NULL);
    }
}

uint16_t audio_player_progress_per_mille(void)
{
    // The streaming task increases bytes_played as it sends sound. The main
    // UI loop reads this tiny snapshot through music_controller_update(), then
    // display_ui_set_progress() moves the one now-playing bar on screen.
    uint32_t total_bytes = current_wav.data_size;
    uint32_t played_bytes = bytes_played;
    if (total_bytes == 0)
    {
        return 0;
    }
    if (played_bytes >= total_bytes)
    {
        return 1000;
    }
    return (uint16_t)((played_bytes * 1000ULL) / total_bytes);
}

const char *audio_player_last_error(void)
{
    // Tiny status pipe back to music_controller_update() and then display_ui.
    // Empty string means no problem right now.
    return last_error;
}
