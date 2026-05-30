// ============================================================================
// visualizer.cpp
//  Low-cost themed voice-recording waveform driven by FFT levels
// ============================================================================
//
// LOOK
// ----
// This version is styled like a voice memo recording display: slim rounded
// pills float around one horizontal center line and stretch equally upward
// and downward as sound grows louder. Each pill is drawn as an upper and a
// lower half: the wallpaper primary color begins at equilibrium, then blends
// into its brighter highlight toward both moving tips.
//
// PERFORMANCE
// -----------
// The connected canvas wave was too much display work. This goes back to the
// efficient approach:
//   - twenty-nine skinny bar pairs created once,
//   - no alpha canvas, polygon fill, blur, glow, or shadow,
//   - maximum 30 geometry updates per second,
//   - unchanged bars are not invalidated at all.
// The maximum amplitude is intentionally dramatic, but it still fits inside
// the clear area between the header and bottom playback controls.
//
// AUDIO LINK
// ----------
// spectrum_analyzer.cpp still performs FFT work away from the UI and publishes
// thirteen bass-to-treble levels. Twenty-nine visual pills are smoothly sampled
// across those values, increasing density without doing a larger FFT. Each
// pill has a tiny fixed response character, so the wave feels alive but still
// comes entirely from music energy rather than random animation.

#include <Arduino.h>
#include <lvgl.h>
#include "accent_colors.h"
#include "spectrum_analyzer.h"
#include "visualizer.h"

// Thirty frames per second makes the taller gradient bars flow between fresh
// FFT snapshots, without asking LVGL to chase a full phone-style 60 fps redraw.
static constexpr uint32_t VISUALIZER_UPDATE_INTERVAL_MS = 33;
static constexpr size_t WAVE_PILL_COUNT = 29;
static constexpr lv_coord_t STAGE_WIDTH = 245;
static constexpr lv_coord_t STAGE_HEIGHT = 150;
static constexpr lv_coord_t PILL_WIDTH = 5;
static constexpr lv_coord_t PILL_GAP = 3;
static constexpr lv_coord_t HALF_MIN_HEIGHT = 2;
static constexpr lv_coord_t HALF_MAX_HEIGHT = 62;
static constexpr lv_coord_t MIDLINE_Y = STAGE_HEIGHT / 2;

static lv_obj_t *visualizer_stage = nullptr;
static lv_obj_t *upper_pills[WAVE_PILL_COUNT] = {};
static lv_obj_t *lower_pills[WAVE_PILL_COUNT] = {};
static uint8_t displayed_levels[WAVE_PILL_COUNT] = {};
static uint32_t last_visualizer_update_ms = 0;

static lv_coord_t pill_x(size_t index)
{
    const lv_coord_t waveform_width =
        (WAVE_PILL_COUNT * PILL_WIDTH) + ((WAVE_PILL_COUNT - 1) * PILL_GAP);
    return ((STAGE_WIDTH - waveform_width) / 2) +
           (index * (PILL_WIDTH + PILL_GAP));
}

static lv_coord_t level_to_half_height(uint8_t level)
{
    return HALF_MIN_HEIGHT +
           ((lv_coord_t)level * (HALF_MAX_HEIGHT - HALF_MIN_HEIGHT) / 100);
}

static uint8_t animate_toward(uint8_t current, uint8_t target, size_t pill_index)
{
    // Adjacent bars share a musical contour, but these small stable variations
    // keep them from rising and dropping as one rigid sheet. They affect only
    // easing, never fabricate movement when the incoming target is silent.
    // These short repeating patterns scale cleanly when the visual density
    // changes; they add fine-grained character without maintaining a large
    // one-value-per-pill table.
    static const uint8_t attack_pattern[] = {72, 62, 70, 58, 75, 64, 69};
    static const uint8_t release_pattern[] = {2, 2, 3, 3, 2, 2, 3};
    uint8_t attack_percent = attack_pattern[pill_index % 7];
    uint8_t release_divisor = release_pattern[pill_index % 7];

    if (target > current)
    {
        uint8_t step = (uint8_t)max(4,
                                    ((int)target - current) * attack_percent / 100);
        return (uint8_t)min((int)target, (int)current + step);
    }
    if (target < current)
    {
        uint8_t step = (uint8_t)max(3,
                                    ((int)current - target + release_divisor - 1) /
                                        release_divisor);
        return (uint8_t)max((int)target, (int)current - step);
    }
    return current;
}

static uint8_t interpolated_level(const uint8_t levels[SPECTRUM_BAND_COUNT], size_t pill_index)
{
    // The FFT deliberately stays at thirteen efficient real frequency bands.
    // Position each visible pill along that existing spectrum and blend the
    // nearest pair. This creates a fuller recording waveform, while preserving
    // the true bass-to-treble contour and avoiding extra analysis cost.
    const uint16_t position_times_100 =
        (uint16_t)(pill_index * (SPECTRUM_BAND_COUNT - 1) * 100 / (WAVE_PILL_COUNT - 1));
    const size_t lower_band = position_times_100 / 100;
    const size_t upper_band = min(lower_band + 1, (size_t)SPECTRUM_BAND_COUNT - 1);
    const uint8_t blend = position_times_100 % 100;
    uint8_t blended = (uint8_t)(((uint16_t)levels[lower_band] * (100 - blend) +
                                 (uint16_t)levels[upper_band] * blend) /
                                100);

    // A subtle fixed gain profile keeps in-between bars individually shaped.
    // The values are deliberately close to 100 percent: frequency content
    // remains recognizable, while a vocal or beat creates nicer local peaks.
    static const uint8_t character_pattern[] = {103, 96, 108, 98, 105, 94, 106, 99, 109};
    return (uint8_t)min(100,
                        ((int)blended * character_pattern[pill_index % 9]) / 100);
}

void visualizer_apply_accent_colors(void)
{
    // Every bar uses the same mirrored palette treatment:
    //   upper tip <- highlight ... primary <- center -> primary ... highlight -> lower tip.
    // LVGL gradients run from an object's top toward its bottom, so the upper
    // half intentionally reverses the color order used by the lower half.
    for (size_t i = 0; i < WAVE_PILL_COUNT; ++i)
    {
        if (upper_pills[i] && lower_pills[i])
        {
            lv_obj_set_style_bg_color(upper_pills[i], lv_color_hex(wallpaper_accents.highlight), LV_PART_MAIN);
            lv_obj_set_style_bg_grad_color(upper_pills[i], lv_color_hex(wallpaper_accents.primary), LV_PART_MAIN);
            lv_obj_set_style_bg_grad_dir(upper_pills[i], LV_GRAD_DIR_VER, LV_PART_MAIN);

            lv_obj_set_style_bg_color(lower_pills[i], lv_color_hex(wallpaper_accents.primary), LV_PART_MAIN);
            lv_obj_set_style_bg_grad_color(lower_pills[i], lv_color_hex(wallpaper_accents.highlight), LV_PART_MAIN);
            lv_obj_set_style_bg_grad_dir(lower_pills[i], LV_GRAD_DIR_VER, LV_PART_MAIN);
        }
    }
}

static lv_obj_t *make_pill_half(void)
{
    lv_obj_t *pill = lv_obj_create(visualizer_stage);
    lv_obj_set_size(pill, PILL_WIDTH, HALF_MIN_HEIGHT);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_radius(pill, PILL_WIDTH / 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(pill, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pill, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, LV_PART_MAIN);
    return pill;
}

void visualizer_create(lv_obj_t *parent)
{
    visualizer_stage = lv_obj_create(parent);
    lv_obj_set_size(visualizer_stage, STAGE_WIDTH, STAGE_HEIGHT);
    lv_obj_align(visualizer_stage, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(visualizer_stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(visualizer_stage, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_bg_opa(visualizer_stage, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(visualizer_stage, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(visualizer_stage, 0, LV_PART_MAIN);

    for (size_t i = 0; i < WAVE_PILL_COUNT; ++i)
    {
        upper_pills[i] = make_pill_half();
        lower_pills[i] = make_pill_half();
        lv_obj_set_pos(upper_pills[i], pill_x(i), MIDLINE_Y - HALF_MIN_HEIGHT);
        lv_obj_set_pos(lower_pills[i], pill_x(i), MIDLINE_Y);
    }

    visualizer_apply_accent_colors();
}

void visualizer_update(void)
{
    if (!visualizer_stage || millis() - last_visualizer_update_ms < VISUALIZER_UPDATE_INTERVAL_MS)
    {
        return;
    }
    last_visualizer_update_ms = millis();

    uint8_t spectrum_levels[SPECTRUM_BAND_COUNT] = {};
    spectrum_analyzer_get_levels(spectrum_levels);

    for (size_t i = 0; i < WAVE_PILL_COUNT; ++i)
    {
        // Edge pills match true FFT bands, while in-between pills blend their
        // neighbours. Different positions still animate differently, but the
        // curve now looks denser than thirteen chunky independent bars.
        uint8_t next = animate_toward(displayed_levels[i],
                                      interpolated_level(spectrum_levels, i),
                                      i);
        if (next == displayed_levels[i])
        {
            continue;
        }

        // Growing two mirrored halves keeps their `primary` color fixed at
        // equilibrium while `highlight` travels outward with the tips.
        displayed_levels[i] = next;
        lv_coord_t half_height = level_to_half_height(next);
        lv_obj_set_height(upper_pills[i], half_height);
        lv_obj_set_y(upper_pills[i], MIDLINE_Y - half_height);
        lv_obj_set_height(lower_pills[i], half_height);
    }
}
