#pragma once

// setup() in src/main.cpp calls this after display_ui_create(), because this
// startup flow immediately writes wallpaper/song status onto the screen.
// It loads/caches wallpaper before opening a WAV to avoid SD card contention.
void music_controller_start(void);

// loop() in src/main.cpp calls this for progress/status and lightweight
// visualizer animation. Audio streaming and FFT work remain on worker tasks.
void music_controller_update(void);

// Previous/next buttons created in display_ui.cpp call these; this module owns
// the current /music index and asks audio_player.cpp to replace the WAV safely.
bool music_controller_previous_song(void);
bool music_controller_next_song(void);
