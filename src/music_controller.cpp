// ============================================================================
// music_controller.cpp
//  Connects SD files, lockscreen wallpaper, WAV playback, and screen messages
// ============================================================================

#include <Arduino.h>
#include <cstring>
#include "audio_player.h"
#include "display_ui.h"
#include "file_browser.h"
#include "music_controller.h"
#include "sd_manager.h"
#include "visualizer.h"

static size_t current_song_index = 1;
static bool music_folder_ready = false;

// This module is the traffic controller, not the hardware driver:
// - display_ui.cpp knows taps/labels/widgets.
// - file_browser.cpp knows which SD paths exist.
// - audio_player.cpp knows how bytes become speaker output.
// Putting track choice here prevents the UI from reaching straight into file
// handles or audio DMA state when someone taps next.
static bool start_song_at_index(size_t song_index)
{
    // Keep path searching here rather than inside a button callback. This is
    // the one road both startup and previous/next use:
    // file_browser.cpp finds a WAV -> audio_player.cpp safely swaps playback
    // -> display_ui.cpp shows the new song and resets the progress bar.
    String song_path = file_browser_find_nth_wav("/music", song_index);
    if (song_path.length() == 0)
    {
        // For next/previous, false means "leave the song currently playing
        // alone". We have not called audio_player_start_wav() yet.
        return false;
    }

    display_ui_set_song(song_path.c_str());
    display_ui_set_progress(0);
    display_ui_set_status("Opening WAV...");
    if (!audio_player_start_wav(song_path))
    {
        display_ui_set_status(audio_player_last_error());
        return false;
    }

    current_song_index = song_index;
    display_ui_set_pause_button_enabled(true);
    display_ui_set_status("Playing");
    return true;
}

void music_controller_start(void)
{
    // Here is the full startup trail:
    // sd_manager.cpp mounts the card and registers image reads for LVGL.
    // file_browser.cpp chooses the wallpaper and second WAV path.
    // display_ui.cpp paints the background/song/status.
    // audio_player.cpp turns the WAV bytes into speaker audio.
    display_ui_set_status("Mounting SD card...");
    if (!sd_manager_mount())
    {
        display_ui_set_status("SD card mount failed");
        display_ui_set_song("/music");
        return;
    }

    // Next linked function: display_ui_set_background() validates the found
    // picture size and displays a helpful warning if it cannot become wallpaper.
    sd_manager_register_lvgl_filesystem();
    String background_path = file_browser_find_background_image("/images");
    if (background_path.length() > 0)
    {
        // Next linked function: display_ui_set_background() checks dimensions,
        // draws the wallpaper once before audio begins, and leaves its decoded
        // pixels cached in PSRAM so this SD card is free for WAV streaming.
        display_ui_set_background(background_path.c_str());
    }
    else
    {
        display_ui_set_wallpaper_message("No image in /images");
    }

    display_ui_set_status("Scanning /music...");

    // Index 1 means your second WAV: index 0 is the first file, index 1 the second.
    // Wallpaper work is complete before this call. That ordering is intentional:
    // decoding a PNG and streaming a WAV through the same SD card at once was
    // the original freeze/click problem.
    music_folder_ready = true;
    if (!start_song_at_index(current_song_index))
    {
        display_ui_set_status("Need 2 WAVs in /music");
        display_ui_set_song("/music");
        return;
    }
}

bool music_controller_previous_song(void)
{
    // This is called from previous_button_event_cb() in display_ui.cpp on the
    // normal LVGL/main-loop side, never inside the audio streaming task.
    if (!music_folder_ready)
    {
        return false;
    }
    if (current_song_index == 0)
    {
        display_ui_set_status("First song");
        return false;
    }

    // Next linked step: start_song_at_index() opens the earlier file and
    // audio_player_start_wav() resets the I2S stream without crackling overlap.
    return start_song_at_index(current_song_index - 1);
}

bool music_controller_next_song(void)
{
    if (!music_folder_ready)
    {
        return false;
    }

    // Look up the next WAV before disturbing the song currently playing. If
    // no later file exists, keep the current music going and just tell you.
    if (!start_song_at_index(current_song_index + 1))
    {
        display_ui_set_status("Last song");
        return false;
    }
    return true;
}

void music_controller_update(void)
{
    // Playback itself now runs in audio_stream_task() inside audio_player.cpp.
    // This UI-side update carries completion/failure words onto the screen and
    // asks visualizer.cpp to animate already-published FFT levels. It never
    // feeds the speaker or performs FFT work itself.
    visualizer_update();

    static uint32_t last_progress_update_ms = 0;
    if (millis() - last_progress_update_ms >= 200)
    {
        // Updating at five frames per second makes the bar feel alive without
        // redrawing the wallpaper constantly. Next link: display_ui.cpp changes
        // the LVGL bar created in display_ui_create().
        last_progress_update_ms = millis();
        display_ui_set_progress(audio_player_progress_per_mille());
    }

    const char *playback_status = audio_player_last_error();
    // last_error is a deliberately tiny message channel from the background
    // audio task to the visible UI. No Serial Monitor is required to know that
    // a file ended or an SD card disappeared.
    if (strcmp(playback_status, "Finished") == 0)
    {
        display_ui_set_status("Finished");
        display_ui_set_pause_button_enabled(false);
    }
    else if (strcmp(playback_status, "SD removed") == 0)
    {
        // This comes from stop_audio_output() in audio_player.cpp after a WAV
        // read fails early. Show the cause without needing Serial Monitor.
        display_ui_set_status("SD removed");
        display_ui_set_pause_button_enabled(false);
    }
}
