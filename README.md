# Handheld Lossless Music Player + Visualizer

This project is a touchscreen music player for the
Waveshare ESP32-S3-Touch-LCD-3.5 board. It plays uncompressed WAV files from
the built-in SD card slot, shows an SD-card wallpaper, creates matching accent
colors from that wallpaper, and provides touch controls.

This guide explains both how the app is organized and why several of the
hardware/performance fixes are necessary.

<p align="center">
  <img src="https://github.com/user-attachments/assets/2d90c297-28cc-4270-8967-0ea1d9b21680" width="45%">
  &nbsp;&nbsp;&nbsp;&nbsp;
  <img src="https://github.com/user-attachments/assets/dcebfcde-111c-45c9-835b-b87aeed3e8c1" width="45%">
</p>



## What Works Right Now

- Wallpaper loaded from the SD card's `/images` folder.
- Automatic cool-toned accents picked from dominant PNG wallpaper colors.
- A compact now-playing screen with a player title, song path, and progress.
- A lightweight live FFT visualizer styled as a themed voice-recording wave.
- Previous, pause/play, and next touch controls.
- A volume slider connected to the ES8311 audio codec.
- Slide-up music drawer with `Playlists` and `Songs` views.
- WAV playback from `/music`, beginning on the second WAV at boot.
- On-screen status reporting instead of requiring Serial Monitor.
- Protection against repeated ticking if the SD card disappears while playing.

## SD Card Layout

Use folders like this:

```text
/
|-- images/
|   `-- lockscreen for mp3.png
`-- music/
    |-- first_song.wav
    |-- second_song.wav
    `-- more_songs.wav
```

The program searches recursively, so subfolders inside `/music` work. Song
order currently follows SD directory traversal order, not alphabetical order.

### Wallpaper Rules

- Put the preferred wallpaper inside `/images`.
- A filename containing `lockscreen` wins if there are multiple images.
- Supported image extensions are `.png`, `.bmp`, and `.jpg`.
- Automatic accent-color sampling currently uses a `.png` wallpaper.
- Keep wallpaper dimensions at or below `320 x 480`.

### WAV Rules

Playback currently accepts:

- Uncompressed PCM WAV format.
- 16-bit samples.
- Mono or stereo.
- Sample rate from 8000 Hz through 96000 Hz.

MP3 is not played by the custom playback path yet, even though the interface
has MP3-player styling.

## Boot Flow

<p align="center">
  <img src="https://github.com/user-attachments/assets/3107ab1a-7591-4024-af52-958efd014236" width="100%" />
  <img src="https://github.com/user-attachments/assets/06a32479-c48b-4580-a3e9-53150665f398" width="100%">
</p>


The startup order is intentional:

1. [main.cpp](src/main.cpp) starts I2C, display hardware, touch, and LVGL.
2. [display_ui.cpp](src/display_ui.cpp) creates visible widgets.
3. [music_controller.cpp](src/music_controller.cpp) mounts the SD card.
4. [file_browser.cpp](src/file_browser.cpp) finds the wallpaper.
5. The wallpaper is decoded, sampled for colors, drawn, and cached.
6. Only after the image work is done does the controller open a WAV.
7. [audio_player.cpp](src/audio_player.cpp) starts independent audio streaming.

The wallpaper is handled before audio because the PNG and WAV live on the same
SD card. Heavy PNG decoding while music streams was causing clicks and
frozen-feeling touch.

## File Map

### `src/main.cpp`

Owns physical board connections and the Arduino lifecycle:

- Pin map for screen, backlight, and I2C.
- Arduino_GFX display object.
- Touch controller callback.
- LVGL draw buffers and display flush callback.
- `setup()` and `loop()`.

It coordinates modules, but does not own song choice, WAV parsing, or layouts.

### `src/display_ui.cpp`

Owns LVGL widgets and touch events:

- Top now-playing header.
- Background image object.
- Playback progress bar.
- Previous, pause/play, and next controls.
- Volume slider.
- Swipe-up drawer.
- Applying wallpaper-derived accents to controls.

A next-button tap travels like this:

```text
display_ui.cpp
  -> music_controller_next_song()
  -> audio_player_start_wav(new path)
  -> safe file and I2S replacement
```

### `src/music_controller.cpp`

This is the middle layer between screen, files, and sound. It owns the current
song index, performs startup in the audio-safe order, and passes progress or
errors from the sound module onto the screen.

### `src/audio_player.cpp`

Owns timing-sensitive sound details:

- WAV header parsing.
- ES8311 codec initialization and volume.
- I2S configuration.
- DMA queue feeding.
- FreeRTOS audio-streaming task.
- Post-DMA PCM handoff for optional background analysis.
- Pause state and playback progress.
- Safe track replacement using a mutex.

This is the key file for understanding how actual sound reaches the speaker.

### `src/spectrum_analyzer.cpp`

Owns the efficient background sound analysis:

- Lower-priority queued 1024-point FFT analysis, after DMA has accepted audio.
- Hann windowing and cached FFT twiddle steps.
- Thirteen lightweight `0..100` bass-to-treble levels for the live visualizer.
- Reset-on-pause/track-change behavior so the animation can settle immediately.

### `src/sd_manager.cpp`

Mounts the physical SD_MMC card and creates LVGL's `S:` filesystem adapter.
Audio can open `/music/song.wav` using SD_MMC, while LVGL asks for
`S:/images/lockscreen.png`; both reach the same card.

### `src/file_browser.cpp`

Searches the SD card for WAV paths and wallpaper paths. It returns names only;
it does not draw or play anything.

### `src/accent_colors.cpp`

Samples decoded PNG pixels during startup and creates four theme roles:

- `primary`: most-used acceptable cool color, for main controls.
- `secondary`: next most-used distinct cool color.
- `highlight`: brighter readable accent for progress and slider details.
- `muted`: darker derived shade for visualizer depth.

Grey/black/white areas and warm red/orange colors are not promoted into UI
accents, because those colors looked out of place on this interface.

### `src/visualizer.cpp`

Creates twenty-nine skinny centered recording-style pills in the open center space.
Every pill is a mirrored palette gradient: `primary` begins at the equilibrium
line and blends to `highlight` at both moving tips. The thirteen efficient FFT
bands remain the real audio data; extra pills interpolate between adjacent
bands for a denser wave without heavier analysis. The display refreshes up to
thirty times per second, with small fixed per-pill response differences to
keep motion lively while preserving the real FFT shape. The maximum centered
amplitude is stretched for more dramatic peaks without adding more FFT work.

### `include/lv_conf.h`

Holds LVGL configuration. Key settings for this app:

- Large LVGL allocations prefer PSRAM.
- One image cache entry keeps the decoded wallpaper available.
- PNG/BMP/JPG support is enabled.
- Only needed Montserrat font sizes are enabled.

## Theory and Troubleshooting

### Why Touching The Screen Corrupted Audio

Initially, audio was fed from the regular Arduino `loop()`, the same loop
responsible for LVGL animations and display drawing. Swiping the drawer makes
LVGL redraw large regions and push pixels to the LCD over SPI. While that work
ran, new sound data was not reaching audio hardware fast enough.

Digital audio needs a steady stream. When queued samples run out, the output
becomes broken or clicky.

The fix was moving WAV streaming into a dedicated FreeRTOS task inside
`audio_player.cpp`. Now the display can be busy while audio continues feeding
I2S independently.

### I2S And DMA In Normal Words

A PCM WAV file is a sequence of sample numbers. Those values must reach the
ES8311 codec with consistent timing.

I2S is the digital audio connection:

- `MCLK` supplies master timing for the codec.
- `BCLK` clocks individual audio bits.
- `LRCK` identifies left/right sample timing.
- `SDOUT` carries the sample data.

DMA is a hardware-fed queue. Software loads chunks of sample data into DMA;
hardware steadily sends them through I2S even if the CPU is briefly busy with
the screen. This project uses a deeper DMA queue to survive short redraw bursts.

DMA handles bumps, not unlimited delays. The separate audio task is what keeps
the queue refilled. Spectrum work now receives an occasional copied sample
frame only after I2S has accepted the audio chunk; its FFT task runs at lower
priority than streaming and keeps only the latest queued frame. The Hann window
and FFT twiddle steps are prepared once so steady-state analysis avoids repeated
trigonometry and unnecessary square-root work.

### Why Removing The SD Card Caused Ticking

If the SD card disappears during playback, the next read cannot obtain fresh
samples. Previously, a small stale audio fragment could remain in output and
sound like a repeating tick.

`stop_audio_output()` now clears the I2S DMA buffer whenever playback ends
unexpectedly or an SD read fails. The screen shows `SD removed` as the error.

Pause or power off before removing the SD card whenever possible.

## Why The Wallpaper Froze The UI Or Hurt Sound

PNG images are compressed. Displaying one requires reading the file and
decoding it into pixels. If that decoding repeatedly accesses the same SD card
while a WAV is streaming, both jobs interfere with each other.

The current approach is:

1. Find the wallpaper during startup.
2. Decode/sample it before opening a WAV.
3. Enable LVGL image caching.
4. Prefer PSRAM for large decoded image memory.

Normal redraws should then reuse cached wallpaper pixels rather than decoding
the PNG repeatedly during playback.

### Why Swiping Was Not Working

The background covers the full screen. In LVGL, a top visual object can catch
the gesture before the screen-level handler sees it.

The wallpaper and visualizer objects now bubble gestures. A swipe that begins
on the image or on a bar still reaches the drawer gesture callback.

### Previous, Next, And Safe Song Switching

The audio task may be reading the current WAV when a touch event asks for the
next file. Closing a file while another task reads it is unsafe.

Song changes now use a mutex:

1. Stop new playback reads.
2. Take the audio mutex.
3. Clear old DMA samples.
4. Close the current WAV.
5. Open and parse the selected WAV.
6. Configure I2S for its sample rate.
7. Resume playback.
8. Release the mutex.

Volume stays at the listener's selected level across track changes.

### Volume And Progress

The volume slider sends a `0..100` value directly to the ES8311 codec. The
codec adjusts output gain, so the ESP32 does not need to rewrite every sample.

The audio task counts played WAV bytes. The screen turns that into one progress
bar, updating at a restrained rate to avoid needless screen work during music.
Long song paths are clipped with dots instead of constantly scrolling, leaving
the animation budget for touch feedback and the compact live visualizer.

### Fonts

The UI currently uses built-in Montserrat sizes enabled in
`include/lv_conf.h`: 12, 14, 16, and 48. Enabling more fonts costs flash.

### `SD card mount failed`

- Power off the board and reinsert the SD card firmly.
- Confirm that the card opens on a computer.
- Keep content inside `/images` and `/music`.

### `Need 2 WAVs in /music`

Boot starts at song index `1`, which means the second WAV file found. Put at
least two valid WAV files inside `/music`, or change `current_song_index` in
`src/music_controller.cpp` to `0`.

### `Use 16-bit PCM WAV`

Export the audio as uncompressed 16-bit PCM WAV.

### Wallpaper does not display

- Use `.png`, `.bmp`, or `.jpg`.
- For automatic accent selection, use `.png`.
- Keep dimensions no larger than `320 x 480`.
- Name the chosen image with `lockscreen` if several pictures are present.

### Audio clicks during interaction

The audio task and DMA changes fix the major cause. The live visualizer remains
limited to a narrow bar region while using up to thirty animation updates per
second; if motion is tuned later, keep the redraw area conservative.

## Good Next Features

- Tune the live visualizer shape and palette on hardware.
- Alphabetically sorted songs.
- Song display names without the `/music/` prefix.
- Persisted volume across power cycles.
- Elapsed and remaining duration labels.

## Reading Order

For a guided tour through the project:

```text
src/main.cpp
  -> src/music_controller.cpp
  -> src/file_browser.cpp and src/sd_manager.cpp
  -> src/display_ui.cpp
  -> src/accent_colors.cpp and src/visualizer.cpp
  -> src/audio_player.cpp and src/spectrum_analyzer.cpp
```

That follows the board from startup, to files, to UI, to actual sound output.
