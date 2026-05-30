// ============================================================================
// accent_colors.cpp
//  Colours sampled from the current wallpaper, shared by controls and waveform
// ============================================================================
//
// Why sample once at startup?
// A real automatic theme should follow the wallpaper, but repeatedly analyzing
// pixels would cost display/SD/audio time. display_ui.cpp opens the decoded
// PNG before music begins, calls this once, then every widget reuses four tiny
// RGB values for the rest of that boot.

#include <Arduino.h>
#include "accent_colors.h"

// These are pleasant startup/failure fallbacks. As soon as
// display_ui_set_background() decodes a valid PNG, the extraction function
// below overwrites them with colours found in your actual wallpaper.
AccentColors wallpaper_accents = {
    0x3983C7, // primary: cool blue while wallpaper is loading
    0x43BBC0, // secondary: cyan companion
    0xBCEFF0, // highlight: pale ice
    0x18344E  // muted: deep blue
};

struct ColourBucket
{
    // Similar sampled pixels land in one rough group. Keeping sums plus a
    // count lets the chosen result be the group's average visible shade.
    uint32_t red_total;
    uint32_t green_total;
    uint32_t blue_total;
    uint16_t samples;
};

static uint32_t make_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    // LVGL's lv_color_hex() expects familiar web-style 0xRRGGBB values.
    return ((uint32_t)red << 16) | ((uint32_t)green << 8) | blue;
}

static uint8_t channel(uint32_t colour, uint8_t shift)
{
    return (uint8_t)((colour >> shift) & 0xFF);
}

static uint32_t soften_colour(uint32_t colour, uint8_t numerator, uint8_t add)
{
    // `muted` is derived from primary instead of sampled separately, so dark
    // bars support the theme without introducing an unrelated fifth colour.
    uint8_t red = (uint8_t)((channel(colour, 16) * numerator) / 100 + add);
    uint8_t green = (uint8_t)((channel(colour, 8) * numerator) / 100 + add);
    uint8_t blue = (uint8_t)((channel(colour, 0) * numerator) / 100 + add);
    return make_rgb(red, green, blue);
}

static bool is_warm_red_or_orange(uint8_t red, uint8_t green, uint8_t blue)
{
    // The wallpaper may contain red/orange detail, but it felt out of place
    // when promoted to UI ink. Reject any clearly red-led warm bucket and let
    // dominant cool colours from the same image provide the controls instead.
    return red > 112 &&
           (int)red > (int)green + 16 &&
           (int)red > (int)blue + 18;
}

static uint32_t cool_companion_from(uint32_t primary, uint8_t brighten)
{
    // If the image only gives one acceptable dominant cool color, make a
    // readable cyan-leaning companion rather than bringing back warm red.
    uint8_t red = min((int)channel(primary, 16) + (brighten / 4), 255);
    uint8_t green = min((int)channel(primary, 8) + brighten, 255);
    uint8_t blue = min((int)channel(primary, 0) + brighten, 255);
    return make_rgb(red, green, blue);
}

void accent_colors_extract_from_rgb565_alpha(const uint8_t *pixels, uint32_t pixel_count)
{
    if (!pixels || pixel_count == 0)
    {
        return;
    }

    // 3 bits per colour channel makes 512 groups (8 red x 8 green x 8 blue).
    // That merges near-identical pixels, so tiny gradient differences do not
    // steal the "most used" prize from the real dominant shade.
    static ColourBucket buckets[512];
    memset(buckets, 0, sizeof(buckets));

    for (uint32_t pixel_index = 0; pixel_index < pixel_count; pixel_index += 23)
    {
        // Sampling every 23rd pixel is plenty to estimate the whole artwork
        // palette without examining all 153,600 pixels on a 320 x 480 screen.
        const uint8_t *pixel = pixels + (pixel_index * 3);
        if (pixel[2] < 96)
        {
            // Transparent pixels are not strongly visible, so they should not
            // decide the control color.
            continue;
        }

        uint16_t rgb565 = (uint16_t)pixel[0] | ((uint16_t)pixel[1] << 8);
        uint8_t red = (uint8_t)(((rgb565 >> 11) & 0x1F) * 255 / 31);
        uint8_t green = (uint8_t)(((rgb565 >> 5) & 0x3F) * 255 / 63);
        uint8_t blue = (uint8_t)((rgb565 & 0x1F) * 255 / 31);
        uint8_t maximum = max(red, max(green, blue));
        uint8_t minimum = min(red, min(green, blue));
        uint8_t saturation = maximum - minimum;
        uint16_t brightness = ((uint16_t)red * 3 + (uint16_t)green * 6 + blue) / 10;

        // Black/white/grey patches are useful wallpaper but poor accent ink.
        // Warm red/orange pixels are also skipped so those wallpaper details
        // remain in the photo without becoming bars, outlines, or buttons.
        if (brightness < 28 || brightness > 244 || saturation < 22 ||
            is_warm_red_or_orange(red, green, blue))
        {
            continue;
        }

        uint16_t bucket_index = ((red >> 5) << 6) | ((green >> 5) << 3) | (blue >> 5);
        ColourBucket &bucket = buckets[bucket_index];
        bucket.red_total += red;
        bucket.green_total += green;
        bucket.blue_total += blue;
        bucket.samples++;
    }

    uint16_t primary_samples = 0;
    uint16_t secondary_samples = 0;
    uint32_t bright_score = 0;
    uint32_t primary = wallpaper_accents.primary;
    uint32_t secondary = wallpaper_accents.secondary;
    uint32_t highlight = wallpaper_accents.highlight;

    // First pass: primary means what it sounds like now: the most frequently
    // sampled usable colour in the wallpaper. We already removed transparent,
    // almost-grey, and warm red/orange buckets above.
    for (int i = 0; i < 512; ++i)
    {
        if (buckets[i].samples == 0)
        {
            continue;
        }

        uint8_t red = buckets[i].red_total / buckets[i].samples;
        uint8_t green = buckets[i].green_total / buckets[i].samples;
        uint8_t blue = buckets[i].blue_total / buckets[i].samples;
        uint8_t maximum = max(red, max(green, blue));
        uint8_t minimum = min(red, min(green, blue));
        uint8_t saturation = maximum - minimum;
        uint16_t brightness = ((uint16_t)red * 3 + (uint16_t)green * 6 + blue) / 10;
        uint32_t colour = make_rgb(red, green, blue);

        if (buckets[i].samples > primary_samples)
        {
            primary = colour;
            primary_samples = buckets[i].samples;
        }

        uint32_t luminous_score = (uint32_t)buckets[i].samples + ((uint32_t)brightness * saturation);
        if (brightness > 115 && luminous_score > bright_score)
        {
            highlight = colour;
            bright_score = luminous_score;
        }
    }

    if (primary_samples == 0)
    {
        return;
    }

    // Second pass: choose the next most-used bucket that is visibly separate
    // from primary. Without the distance check two neighboring shades of the
    // same colour could take both accent slots and look accidental.
    for (int i = 0; i < 512; ++i)
    {
        if (buckets[i].samples == 0)
        {
            continue;
        }

        uint8_t red = buckets[i].red_total / buckets[i].samples;
        uint8_t green = buckets[i].green_total / buckets[i].samples;
        uint8_t blue = buckets[i].blue_total / buckets[i].samples;
        int distance = abs((int)red - (int)channel(primary, 16)) +
                       abs((int)green - (int)channel(primary, 8)) +
                       abs((int)blue - (int)channel(primary, 0));
        if (distance > 70 && buckets[i].samples > secondary_samples)
        {
            secondary = make_rgb(red, green, blue);
            secondary_samples = buckets[i].samples;
        }
    }

    wallpaper_accents.primary = primary;
    wallpaper_accents.secondary = (secondary_samples &&
                                   !is_warm_red_or_orange(channel(secondary, 16), channel(secondary, 8), channel(secondary, 0)))
                                      ? secondary
                                      : cool_companion_from(primary, 32);
    wallpaper_accents.highlight = (bright_score &&
                                   !is_warm_red_or_orange(channel(highlight, 16), channel(highlight, 8), channel(highlight, 0)))
                                      ? highlight
                                      : cool_companion_from(primary, 72);
    wallpaper_accents.muted = soften_colour(primary, 38, 8);
    // Next linked step: display_ui.cpp reads these four fields in
    // apply_wallpaper_accent_colors(), then asks visualizer.cpp to repaint.
}
