// ============================================================================
// display_ui.cpp
//  The lock screen plus the slide-up music drawer
// ============================================================================
//
// UI LAYER ORDER
// --------------
// LVGL objects created later sit visually above objects created earlier:
//   background image -> live visualizer -> status/header -> transport controls
//   -> slide-up drawer.
// The background and marker still set LV_OBJ_FLAG_EVENT_BUBBLE so a swipe that
// begins on visible content reaches the screen gesture handler underneath.
//
// Rendering warning for this project: every changed LVGL object eventually
// becomes SPI pixel traffic in my_disp_flush() in main.cpp. Moving large
// transparent panels constantly is expensive, so audio has its own task and
// the live visualizer updates only a tiny fixed group of bar objects.

#include <lvgl.h>
#include <stdint.h>
#include "accent_colors.h"
#include "audio_player.h"
#include "display_ui.h"
#include "music_controller.h"
#include "visualizer.h"

static constexpr lv_coord_t SCREEN_WIDTH = 320;
static constexpr lv_coord_t SCREEN_HEIGHT = 480;
static constexpr lv_coord_t DRAWER_OPEN_Y = 58;
static constexpr lv_coord_t DRAWER_CLOSED_Y = SCREEN_HEIGHT;

// Each value is a left-side drawer button. Tapping one travels through
// menu_row_event_cb() -> show_music_view() to update the right-side panel.
enum MusicView
{
    VIEW_PLAYLISTS,
    VIEW_SONGS,
    VIEW_COUNT
};

static lv_obj_t *lockscreen_background = nullptr;
static lv_obj_t *wallpaper_message_label = nullptr;
// These live on the home/lock screen now. main.cpp updates song/status, and
// pause_button_event_cb() asks audio_player.cpp to pause or resume playback.
static lv_obj_t *status_label = nullptr;
static lv_obj_t *song_label = nullptr;
static lv_obj_t *playback_progress = nullptr;
static lv_obj_t *pause_button = nullptr;
static lv_obj_t *pause_button_label = nullptr;
static lv_obj_t *previous_button = nullptr;
static lv_obj_t *next_button = nullptr;
static lv_obj_t *volume_slider = nullptr;
static lv_obj_t *swipe_handle = nullptr;
static lv_obj_t *music_drawer = nullptr;
static lv_obj_t *menu_rows[VIEW_COUNT] = {};
static lv_obj_t *menu_labels[VIEW_COUNT] = {};
static lv_obj_t *menu_arrows[VIEW_COUNT] = {};
static lv_obj_t *panel_title = nullptr;
static lv_obj_t *panel_body = nullptr;
static MusicView active_view = VIEW_PLAYLISTS;
static bool drawer_open = false;

static void show_music_view(MusicView view);

static void animate_drawer_to(lv_coord_t y)
{
    // Let the menu move like a phone drawer instead of abruptly teleporting in.
    // Each animation frame invalidates display pixels and later reaches
    // my_disp_flush() in main.cpp. Audio stays steady because DMA is fed
    // independently by audio_stream_task() in audio_player.cpp.
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, music_drawer);
    lv_anim_set_values(&anim, lv_obj_get_y(music_drawer), y);
    lv_anim_set_time(&anim, 230);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, [](void *obj, int32_t value) {
        lv_obj_set_y((lv_obj_t *)obj, (lv_coord_t)value);
    });
    lv_anim_start(&anim);
}

static void gesture_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_GESTURE)
    {
        return;
    }

    // LVGL reaches this function from lv_timer_handler() in main.cpp after
    // my_touchpad_read() has reported a swipe from the real touch controller.
    lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (direction == LV_DIR_TOP && !drawer_open)
    {
        drawer_open = true;
        animate_drawer_to(DRAWER_OPEN_Y);
    }
    else if (direction == LV_DIR_BOTTOM && drawer_open)
    {
        drawer_open = false;
        animate_drawer_to(DRAWER_CLOSED_Y);
    }
}

void display_ui_set_status(const char *text)
{
    // music_controller.cpp uses this while loading/finishing a song, and the pause callback
    // below reuses it so every playback message lands in the same spot.
    if (status_label)
    {
        lv_label_set_text(status_label, text);
    }
}

void display_ui_set_song(const char *text)
{
    // music_controller_start() in src/music_controller.cpp hands us the path selected
    // by file_browser_find_nth_wav(), such as /music/second_song.wav.
    if (song_label)
    {
        lv_label_set_text(song_label, text);
    }
}

void display_ui_set_progress(uint16_t progress_per_mille)
{
    if (playback_progress)
    {
        // music_controller.cpp gives us the bytes-played position from
        // audio_player.cpp. This is intentionally the only progress/volume
        // line in the now-playing layout, matching the clean player reference.
        // Avoid animating between values: extra tween frames would redraw the
        // header repeatedly for a tiny detail while the device is playing music.
        lv_bar_set_value(playback_progress, progress_per_mille, LV_ANIM_OFF);
    }
}

static void apply_wallpaper_accent_colors(void)
{
    // accent_colors.cpp fills wallpaper_accents from decoded PNG pixels. This
    // function is the next stop: repaint the controls which already existed
    // before music_controller.cpp found and loaded the SD wallpaper.
    if (playback_progress)
    {
        lv_obj_set_style_bg_color(playback_progress, lv_color_hex(wallpaper_accents.highlight), LV_PART_INDICATOR);
    }
    if (pause_button)
    {
        lv_obj_set_style_bg_color(pause_button, lv_color_hex(wallpaper_accents.primary), LV_PART_MAIN);
    }
    if (previous_button)
    {
        lv_obj_set_style_border_color(previous_button, lv_color_hex(wallpaper_accents.secondary), LV_PART_MAIN);
    }
    if (next_button)
    {
        lv_obj_set_style_border_color(next_button, lv_color_hex(wallpaper_accents.secondary), LV_PART_MAIN);
    }
    if (volume_slider)
    {
        lv_obj_set_style_bg_color(volume_slider, lv_color_hex(wallpaper_accents.secondary), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(volume_slider, lv_color_hex(wallpaper_accents.highlight), LV_PART_KNOB);
    }
    if (swipe_handle)
    {
        lv_obj_set_style_bg_color(swipe_handle, lv_color_hex(wallpaper_accents.secondary), LV_PART_MAIN);
    }

    // The visualizer owns its bars, so ask it to repaint with these accents.
    visualizer_apply_accent_colors();
    show_music_view(active_view);
}

void display_ui_set_wallpaper_message(const char *text)
{
    if (!wallpaper_message_label)
    {
        return;
    }

    // Loading/failure messages appear on-screen because this project does not
    // depend on opening Serial Monitor to diagnose missing SD artwork.
    lv_label_set_text(wallpaper_message_label, text ? text : "");
    if (text && text[0] != '\0')
    {
        lv_obj_clear_flag(wallpaper_message_label, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(wallpaper_message_label, LV_OBJ_FLAG_HIDDEN);
    }
}

bool display_ui_set_background(const char *image_path)
{
    if (!lockscreen_background || !image_path || image_path[0] == '\0')
    {
        display_ui_set_wallpaper_message("Wallpaper path missing");
        return false;
    }

    // "S:" points at the SD card driver set up by
    // sd_manager_register_lvgl_filesystem() in src/sd_manager.cpp.
    // The picture is kept behind the clock and drawer, like actual wallpaper.
    static String lvgl_path;
    lvgl_path = String("S:") + image_path;

    // First ask LVGL for the dimensions without decoding every pixel. Your LCD
    // is 320 x 480; a huge phone/tablet PNG cannot be safely shrunk by this
    // widget. Next practical step is putting a 320 x 480 copy in SD /images.
    lv_img_header_t header;
    if (lv_img_decoder_get_info(lvgl_path.c_str(), &header) != LV_RES_OK)
    {
        display_ui_set_wallpaper_message("Wallpaper format failed");
        return false;
    }
    if (header.w > SCREEN_WIDTH || header.h > SCREEN_HEIGHT)
    {
        display_ui_set_wallpaper_message("Wallpaper too big\nuse 320 x 480");
        return false;
    }

    // PNG is already decoded for drawing, and this happens before
    // audio_player_start_wav() begins using the SD card. Sample those decoded
    // pixels once so accents follow any new wallpaper automatically, then close
    // this temporary decoder. The regular render below is what LVGL caches.
    String lower_path = lvgl_path;
    lower_path.toLowerCase();
    if (lower_path.endsWith(".png"))
    {
        // On this LVGL 16-bit configuration, a decoded PNG uses three bytes
        // per pixel: two RGB565 color bytes followed by alpha. That is the
        // exact layout accent_colors.cpp samples in the next call.
        lv_img_decoder_dsc_t sampled_image;
        if (lv_img_decoder_open(&sampled_image, lvgl_path.c_str(), lv_color_black(), 0) == LV_RES_OK)
        {
            accent_colors_extract_from_rgb565_alpha(sampled_image.img_data,
                                                    (uint32_t)header.w * (uint32_t)header.h);
            lv_img_decoder_close(&sampled_image);
            apply_wallpaper_accent_colors();
        }
    }

    // PNG decoding is slow enough to interrupt streamed sound if it first
    // happens after the song starts. Show a small loading note, then force
    // the wallpaper's first render now while music_controller.cpp is still
    // waiting to open the WAV. LVGL's one-entry image cache keeps this
    // decoded wallpaper in PSRAM for later clock and drawer redraws.
    display_ui_set_wallpaper_message("Loading wallpaper...");
    lv_refr_now(NULL);
    lv_img_set_src(lockscreen_background, lvgl_path.c_str());
    lv_obj_center(lockscreen_background);
    lv_obj_move_background(lockscreen_background);
    // Force the render before music_controller_start() opens a WAV. With the
    // single image cache set in lv_conf.h, this decoded wallpaper stays in
    // PSRAM instead of reopening the SD file on every UI redraw.
    lv_refr_now(NULL);
    display_ui_set_wallpaper_message("");
    return true;
}

void display_ui_set_pause_button_enabled(bool enabled)
{
    if (!pause_button)
    {
        return;
    }

    // Disabled means LVGL visibly refuses the control after playback ends or
    // startup fails, rather than allowing a button tap with no active stream.
    if (enabled)
    {
        lv_obj_clear_state(pause_button, LV_STATE_DISABLED);
    }
    else
    {
        lv_obj_add_state(pause_button, LV_STATE_DISABLED);
    }
}

static void pause_button_event_cb(lv_event_t *event)
{
    // This is linked from the button created near the bottom of
    // display_ui_create(). The sound-side state change happens next inside
    // audio_player_toggle_pause() in src/audio_player.cpp.
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !audio_player_toggle_pause())
    {
        return;
    }

    if (audio_player_is_paused())
    {
        lv_label_set_text(pause_button_label, LV_SYMBOL_PLAY);
        display_ui_set_status("Paused");
    }
    else
    {
        lv_label_set_text(pause_button_label, LV_SYMBOL_PAUSE);
        display_ui_set_status("Playing");
    }
}

static void previous_button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED)
    {
        // music_controller.cpp owns the song index; it passes the older WAV
        // next into audio_player_start_wav(), which swaps it under an audio lock.
        if (music_controller_previous_song())
        {
            lv_label_set_text(pause_button_label, LV_SYMBOL_PAUSE);
        }
    }
}

static void next_button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED)
    {
        // Same route as previous, but searches for the following WAV in /music.
        if (music_controller_next_song())
        {
            lv_label_set_text(pause_button_label, LV_SYMBOL_PAUSE);
        }
    }
}

static void volume_slider_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED)
    {
        // This LVGL value travels directly into the codec register through
        // audio_player_set_volume() in audio_player.cpp; no Serial controls needed.
        // Slider and ES8311 driver share a convenient 0..100 volume scale.
        audio_player_set_volume((uint8_t)lv_slider_get_value((lv_obj_t *)lv_event_get_target(event)));
    }
}

static void show_music_view(MusicView view)
{
    static const char *titles[VIEW_COUNT] = {
        "PLAYLISTS", "SONGS"};
    static const char *descriptions[VIEW_COUNT] = {
        "Saved playlists\nfrom /music",
        "Songs loaded\nfrom SD card"};

    // menu_row_event_cb() passes in the enum belonging to the row that was
    // touched; active_view stores which blue highlight is currently selected.
    active_view = view;
    for (int i = 0; i < VIEW_COUNT; i++)
    {
        bool selected = i == (int)view;
        lv_obj_set_style_bg_color(menu_rows[i],
                                  selected ? lv_color_hex(wallpaper_accents.primary) : lv_color_hex(0x000000),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(menu_rows[i], selected ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_text_color(menu_labels[i],
                                    selected ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xD6DFEA),
                                    LV_PART_MAIN);
        if (selected)
        {
            lv_obj_clear_flag(menu_arrows[i], LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(menu_arrows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_label_set_text(panel_title, titles[view]);
    // Each library section loads into this same neat little right pane.
    // The actual playing song now stays on the lockscreen where it belongs.
    lv_label_set_text(panel_body, descriptions[view]);
}

static void menu_row_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED)
    {
        // make_menu_row() stashed the row's MusicView in user_data below.
        // Pull it back out here and let show_music_view() redraw the panel.
        show_music_view((MusicView)(intptr_t)lv_event_get_user_data(event));
    }
}

static lv_obj_t *make_menu_row(lv_obj_t *parent, const char *text, lv_coord_t y, MusicView view)
{
    // Use actual LVGL buttons instead of checking touch coordinates by hand.
    // LVGL detects the click; `view` rides along in event user_data.
    lv_obj_t *row = lv_btn_create(parent);
    lv_obj_set_size(row, 126, 35);
    lv_obj_set_pos(row, 0, y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, menu_row_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)view);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xD6DFEA), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 9, 0);

    lv_obj_t *arrow = lv_label_create(row);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -7, 0);
    lv_obj_add_flag(arrow, LV_OBJ_FLAG_HIDDEN);

    menu_rows[view] = row;
    menu_labels[view] = label;
    menu_arrows[view] = arrow;
    return row;
}

static void build_music_drawer(void)
{
    // This whole object starts just below the screen. gesture_event_cb()
    // animates it upward when you swipe on the screen created below.
    music_drawer = lv_obj_create(lv_scr_act());
    lv_obj_set_size(music_drawer, SCREEN_WIDTH, SCREEN_HEIGHT - DRAWER_OPEN_Y);
    lv_obj_set_pos(music_drawer, 0, DRAWER_CLOSED_Y);
    lv_obj_clear_flag(music_drawer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(music_drawer, gesture_event_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_set_style_radius(music_drawer, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(music_drawer, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(music_drawer, lv_color_hex(0x4A5666), LV_PART_MAIN);
    lv_obj_set_style_bg_color(music_drawer, lv_color_hex(0x05080E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(music_drawer, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_pad_all(music_drawer, 0, LV_PART_MAIN);

    lv_obj_t *handle = lv_obj_create(music_drawer);
    lv_obj_set_size(handle, 44, 4);
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_radius(handle, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(handle, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(handle, lv_color_hex(0xB7C3D1), LV_PART_MAIN);

    lv_obj_t *bar = lv_obj_create(music_drawer);
    lv_obj_set_size(bar, SCREEN_WIDTH - 2, 37);
    lv_obj_set_pos(bar, 1, 19);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x161D26), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_70, LV_PART_MAIN);

    lv_obj_t *bar_title = lv_label_create(bar);
    lv_label_set_text(bar_title, "Music");
    lv_obj_set_style_text_color(bar_title, lv_color_hex(0xD7E0EA), LV_PART_MAIN);
    lv_obj_align(bar_title, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *battery = lv_obj_create(bar);
    lv_obj_set_size(battery, 25, 12);
    lv_obj_align(battery, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_radius(battery, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(battery, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(battery, lv_color_hex(0xAFBAC7), LV_PART_MAIN);
    lv_obj_set_style_pad_all(battery, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(battery, lv_color_hex(0xA6D16C), LV_PART_MAIN);

    lv_obj_t *menu = lv_obj_create(music_drawer);
    lv_obj_set_size(menu, 127, 310);
    lv_obj_set_pos(menu, 1, 57);
    lv_obj_clear_flag(menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(menu, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(menu, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(menu, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(menu, LV_OPA_TRANSP, LV_PART_MAIN);

    make_menu_row(menu, "Playlists", 0, VIEW_PLAYLISTS);
    make_menu_row(menu, "Songs", 35, VIEW_SONGS);

    lv_obj_t *content_panel = lv_obj_create(music_drawer);
    lv_obj_set_size(content_panel, 191, 310);
    lv_obj_set_pos(content_panel, 128, 57);
    lv_obj_clear_flag(content_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(content_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(content_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(content_panel, LV_OPA_TRANSP, LV_PART_MAIN);

    panel_title = lv_label_create(content_panel);
    lv_label_set_text(panel_title, "PLAYLISTS");
    lv_obj_set_style_text_color(panel_title, lv_color_hex(0xA6BAD0), LV_PART_MAIN);
    lv_obj_align(panel_title, LV_ALIGN_TOP_LEFT, 2, 12);

    panel_body = lv_label_create(content_panel);
    lv_label_set_text(panel_body, "");
    lv_obj_set_style_text_color(panel_body, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(panel_body, LV_ALIGN_TOP_LEFT, 2, 48);

    show_music_view(VIEW_PLAYLISTS);
}

void display_ui_create(void)
{
    // setup() in main.cpp calls this before trying the SD card or starting
    // music, so the status setters already have labels ready to update.
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x080D14), LV_PART_MAIN);
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(lv_scr_act(), gesture_event_cb, LV_EVENT_GESTURE, NULL);

    // Empty until SD is mounted. Later the route is:
    // music_controller.cpp -> file_browser_find_background_image() -> display_ui_set_background().
    lockscreen_background = lv_img_create(lv_scr_act());
    lv_obj_center(lockscreen_background);
    // When this eventually fills the screen, it must not swallow the swipe.
    // Gesture events bubble back to lv_scr_act(), whose callback opens the drawer.
    lv_obj_add_flag(lockscreen_background, LV_OBJ_FLAG_EVENT_BUBBLE);

    // This stage lives above the wallpaper but below the controls. Follow into
    // visualizer.cpp: tiny live bars consume background FFT levels without
    // making the display own any audio work.
    visualizer_create(lv_scr_act());

    // Hidden unless wallpaper loading fails. This is intentionally on-screen
    // because you asked to avoid depending on Serial Monitor for debugging.
    wallpaper_message_label = lv_label_create(lv_scr_act());
    lv_label_set_text(wallpaper_message_label, "");
    lv_obj_set_width(wallpaper_message_label, 270);
    lv_obj_set_style_text_align(wallpaper_message_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(wallpaper_message_label, lv_color_hex(wallpaper_accents.highlight), LV_PART_MAIN);
    lv_obj_align(wallpaper_message_label, LV_ALIGN_TOP_MID, 0, 196);
    lv_obj_add_flag(wallpaper_message_label, LV_OBJ_FLAG_HIDDEN);

    // A shallow translucent header keeps text readable but leaves most of
    // the wallpaper visible. The open middle is saved for the compact live
    // bass-to-treble visualizer.
    lv_obj_t *now_playing_header = lv_obj_create(lv_scr_act());
    lv_obj_set_size(now_playing_header, SCREEN_WIDTH, 105);
    lv_obj_set_pos(now_playing_header, 0, 0);
    lv_obj_clear_flag(now_playing_header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(now_playing_header, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_radius(now_playing_header, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(now_playing_header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(now_playing_header, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(now_playing_header, lv_color_hex(0x05080E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(now_playing_header, LV_OPA_60, LV_PART_MAIN);

    lv_obj_t *title_label = lv_label_create(now_playing_header);
    lv_label_set_text(title_label, "sahana's muzic player");
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 8);

    // A fixed title avoids periodic clock redraws while playback is busy.
    song_label = lv_label_create(now_playing_header);
    lv_label_set_text(song_label, "/music");
    // Sacrifice marquee motion for responsiveness: a long filename is clipped
    // with dots instead of constantly invalidating header pixels during audio.
    lv_label_set_long_mode(song_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(song_label, 250);
    lv_obj_set_style_text_align(song_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(song_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(song_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(song_label, LV_ALIGN_TOP_MID, 0, 33);

    status_label = lv_label_create(now_playing_header);
    lv_label_set_text(status_label, "Looking for music...");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xC2CEDB), LV_PART_MAIN);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 53);

    playback_progress = lv_bar_create(now_playing_header);
    lv_obj_set_size(playback_progress, 286, 6);
    lv_obj_align(playback_progress, LV_ALIGN_BOTTOM_MID, 0, -13);
    lv_bar_set_range(playback_progress, 0, 1000);
    lv_bar_set_value(playback_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(playback_progress, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(playback_progress, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(playback_progress, lv_color_hex(0x505962), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(playback_progress, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_bg_color(playback_progress, lv_color_hex(wallpaper_accents.highlight), LV_PART_INDICATOR);

    // The middle stays devoted to the wallpaper/visualizer. At the bottom the
    // three familiar transport buttons share one row, with one volume slider.
    previous_button = lv_btn_create(lv_scr_act());
    lv_obj_set_size(previous_button, 50, 50);
    lv_obj_align(previous_button, LV_ALIGN_BOTTOM_MID, -86, -72);
    lv_obj_add_event_cb(previous_button, previous_button_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_radius(previous_button, 25, LV_PART_MAIN);
    lv_obj_set_style_bg_color(previous_button, lv_color_hex(0x05080E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(previous_button, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(previous_button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(previous_button, lv_color_hex(wallpaper_accents.secondary), LV_PART_MAIN);

    lv_obj_t *previous_label = lv_label_create(previous_button);
    lv_label_set_text(previous_label, LV_SYMBOL_PREV);
    lv_obj_set_style_text_color(previous_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(previous_label);

    pause_button = lv_btn_create(lv_scr_act());
    lv_obj_set_size(pause_button, 62, 62);
    lv_obj_align(pause_button, LV_ALIGN_BOTTOM_MID, 0, -66);
    lv_obj_add_event_cb(pause_button, pause_button_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(pause_button, LV_STATE_DISABLED);
    lv_obj_set_style_radius(pause_button, 31, LV_PART_MAIN);
    lv_obj_set_style_bg_color(pause_button, lv_color_hex(wallpaper_accents.primary), LV_PART_MAIN);

    pause_button_label = lv_label_create(pause_button);
    lv_label_set_text(pause_button_label, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_color(pause_button_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(pause_button_label);

    next_button = lv_btn_create(lv_scr_act());
    lv_obj_set_size(next_button, 50, 50);
    lv_obj_align(next_button, LV_ALIGN_BOTTOM_MID, 86, -72);
    lv_obj_add_event_cb(next_button, next_button_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_radius(next_button, 25, LV_PART_MAIN);
    lv_obj_set_style_bg_color(next_button, lv_color_hex(0x05080E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(next_button, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(next_button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(next_button, lv_color_hex(wallpaper_accents.secondary), LV_PART_MAIN);

    lv_obj_t *next_label = lv_label_create(next_button);
    lv_label_set_text(next_label, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_color(next_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(next_label);

    lv_obj_t *volume_icon = lv_label_create(lv_scr_act());
    lv_label_set_text(volume_icon, LV_SYMBOL_VOLUME_MID);
    lv_obj_set_style_text_color(volume_icon, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(volume_icon, LV_ALIGN_BOTTOM_LEFT, 21, -27);

    volume_slider = lv_slider_create(lv_scr_act());
    lv_obj_set_size(volume_slider, 245, 6);
    lv_obj_align(volume_slider, LV_ALIGN_BOTTOM_RIGHT, -17, -33);
    lv_slider_set_range(volume_slider, 0, 100);
    // This default matches AUDIO_START_VOLUME in audio_player.cpp. A drag
    // immediately travels through volume_slider_event_cb() into the codec.
    lv_slider_set_value(volume_slider, 70, LV_ANIM_OFF);
    lv_obj_add_event_cb(volume_slider, volume_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_radius(volume_slider, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(volume_slider, lv_color_hex(0x505962), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(volume_slider, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_bg_color(volume_slider, lv_color_hex(wallpaper_accents.secondary), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volume_slider, lv_color_hex(wallpaper_accents.highlight), LV_PART_KNOB);

    swipe_handle = lv_obj_create(lv_scr_act());
    lv_obj_set_size(swipe_handle, 48, 4);
    lv_obj_align(swipe_handle, LV_ALIGN_BOTTOM_MID, 0, -7);
    lv_obj_set_style_radius(swipe_handle, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(swipe_handle, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(swipe_handle, lv_color_hex(wallpaper_accents.secondary), LV_PART_MAIN);

    build_music_drawer();
}
