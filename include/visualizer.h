#pragma once

#include <lvgl.h>

// display_ui_create() calls this once after creating the wallpaper object.
// It builds centered mirrored-gradient pills in a recording-wave style.
void visualizer_create(lv_obj_t *parent);

// display_ui_set_background() calls this after extracting wallpaper colors.
void visualizer_apply_accent_colors(void);

// music_controller_update() calls this frequently; the implementation rate
// limits itself and reads the cheap FFT result published by spectrum_analyzer.
void visualizer_update(void);
