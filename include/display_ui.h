#pragma once

// setup() in src/main.cpp builds the widgets once; the setters below update
// those same saved widget pointers while music code keeps running.
// Widget callbacks call music_controller.cpp/audio_player.cpp for actions;
// this header intentionally exposes screen changes, not hardware internals.
void display_ui_create(void);

// Playback startup/end and pause taps all report what is happening through this.
void display_ui_set_status(const char *text);
void display_ui_set_song(const char *text);

// music_controller_update() feeds this from audio_player_progress_per_mille();
// 0 starts the song at the left edge and 1000 fills the single playback bar.
void display_ui_set_progress(uint16_t progress_per_mille);

// The path comes from music_controller_start() after file_browser.cpp finds it.
// This validates the file before showing it behind the player widgets.
// For PNG, it also triggers accent sampling before WAV playback begins.
bool display_ui_set_background(const char *image_path);

// music_controller.cpp calls this for missing wallpaper; set_background()
// reuses it for format/size errors so they show on the touchscreen, not Serial.
void display_ui_set_wallpaper_message(const char *text);

// Enabled once audio_player_start_wav() succeeds, disabled when the song ends.
void display_ui_set_pause_button_enabled(bool enabled);
