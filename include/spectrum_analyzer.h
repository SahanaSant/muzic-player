#pragma once

#include <Arduino.h>

// Background FFT output ordered from bass to treble. Unlike the older MIDI
// project, these values come from measured WAV frequency energy rather than
// known MIDI note messages. Levels are scaled 0..100 for a cheap UI read.
static constexpr size_t SPECTRUM_BAND_COUNT = 13;

// Starts the low-priority analysis worker and one-frame queue.
// Playback can continue normally if this optional worker cannot be started.
bool spectrum_analyzer_begin(void);

// Invalidates queued analysis and publishes silence immediately.
void spectrum_analyzer_reset(void);

// Audio calls this only after I2S DMA has accepted a PCM block. The analyzer
// takes sparse mono snapshots so its FFT never sits in playback's critical path.
void spectrum_analyzer_submit_pcm(const uint8_t *pcm_bytes,
                                  size_t byte_count,
                                  uint16_t channels,
                                  uint32_t sample_rate);

// visualizer.cpp reads these levels without touching audio timing.
void spectrum_analyzer_get_levels(uint8_t levels[SPECTRUM_BAND_COUNT]);
