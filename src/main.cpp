// ============================================================================
//  main.cpp 
//  coordinates everything
// ============================================================================

// ============================================================================
//  LIBRARIES
//  Display driver, UI framework, GPIO expander, and capacitive touch driver
// ============================================================================
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include "TCA9554.h"
#include "TouchDrvFT6X36.hpp"
#include "display_ui.h"
#include "music_controller.h"

// main.cpp stays on hardware wiring and scheduling. Follow the feature code to:
// music_controller.cpp for SD + song startup, display_ui.cpp for widgets,
// and audio_player.cpp for actual sound bytes.
//
// A useful way to picture this app:
//   setup() = plug every physical subsystem in and build the first screen once.
//   loop()  = keep the interface responsive; it is not responsible for pouring
//             audio bytes anymore, because audio_player.cpp has its own task.
// That split is why dragging controls can no longer starve the speaker.

// ============================================================================
//  BOARD PIN MAP
//  Physical connections used by the Waveshare ESP32-S3-Touch-LCD-3.5 board
// ============================================================================
#define GFX_BL 6

#define SPI_MISO 2
#define SPI_MOSI 1
#define SPI_SCLK 5

#define LCD_CS -1
#define LCD_DC 3
#define LCD_RST -1
#define LCD_HOR_RES 320
#define LCD_VER_RES 480

#define I2C_SDA 8
#define I2C_SCL 7

// ============================================================================
//  HARDWARE OBJECTS
//  Low-level devices that let the ESP32 talk to the screen and touch panel
// ============================================================================
TCA9554 TCA(0x20);
TouchDrvFT6X36 touch;

// The screen is not connected like a desktop monitor. Arduino_GFX pushes pixel
// data over SPI: a serial stream of bytes sent down physical wires. LCD_DC says
// whether a transfer is a command or image data; CS is unused because this
// board wires the panel as the permanently-selected SPI screen.
Arduino_DataBus *bus = new Arduino_ESP32SPI(
    LCD_DC /* DC */, LCD_CS /* CS */, SPI_SCLK /* SCK */, SPI_MOSI /* MOSI */, SPI_MISO /* MISO */);
Arduino_GFX *gfx = new Arduino_ST7796(
    bus, LCD_RST /* RST */, 0 /* rotation */, true /* IPS */, LCD_HOR_RES, LCD_VER_RES);
 
// ============================================================================
//  LVGL STATE
//  Buffers and display driver state owned by the main coordinator
// ============================================================================

uint32_t screenWidth;
uint32_t screenHeight;
uint32_t bufSize;
lv_disp_draw_buf_t draw_buf;
lv_color_t *disp_draw_buf1;
lv_color_t *disp_draw_buf2;
lv_disp_drv_t disp_drv;

// LVGL does not hold a full second screen image here. Instead it renders into
// strips, then calls my_disp_flush() to send each changed strip to the LCD.
// Two buffers mean LVGL can prepare the next strip while the driver finishes
// handing off the previous one. Audio is kept independent because any big
// LCD transfer can still occupy CPU time for a moment.

// ============================================================================
//  DISPLAY HARDWARE HELPERS
//  Panel reset and the bridge from LVGL pixels to the physical LCD
// ============================================================================
void lcd_reset(void)
{
    // On this board the display reset pin is behind the TCA9554 I/O expander,
    // not directly on the ESP32. The high-low-high pulse is the hardware
    // equivalent of turning the LCD controller off and cleanly back on.
    TCA.write1(1, 1);
    delay(10);
    TCA.write1(1, 0);
    delay(10);
    TCA.write1(1, 1);
    delay(200);
}

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    // LVGL gives us exactly the rectangular region that changed. Sending only
    // that rectangle is much cheaper than redrawing all 320 x 480 pixels for
    // every moving progress or visual element.
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
    // This acknowledgement matters: until LVGL sees "ready", it considers
    // color_p busy and will not reuse that draw buffer for another region.
    lv_disp_flush_ready(disp);
}

// ============================================================================
//  TOUCH INPUT
//  Reads the capacitive touch controller and hands coordinates to LVGL
// ============================================================================
void my_touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    LV_UNUSED(indev_drv);
    int16_t x[1], y[1];
    uint8_t touched = touch.getPoint(x, y, 1);

    if (touched)
    {
        // LVGL does the gesture recognition later. Our job here is just to
        // report "finger down at this coordinate" every time it asks.
        data->state = LV_INDEV_STATE_PR;
        data->point.x = x[0];
        data->point.y = y[0];
    }
    else
    {
        // The release transition lets LVGL finish a click or calculate the
        // direction of a swipe. See gesture_event_cb() in display_ui.cpp next.
        data->state = LV_INDEV_STATE_REL;
    }
}


// ============================================================================
//  STARTUP
//  Bring up the board, initialize LVGL, register drivers, and create the UI
// ============================================================================
void setup(void)
{
    // I2C is the shared slow control bus for touch, GPIO expander, and codec.
    Wire.begin(I2C_SDA, I2C_SCL);

    TCA.begin();
    TCA.pinMode1(1, OUTPUT);
    lcd_reset();

    if (!touch.begin(Wire, FT6X36_SLAVE_ADDRESS))
    {
        while (1)
        {
            delay(1000);
        }
    }

    gfx->begin();
    gfx->fillScreen(BLACK);

#ifdef GFX_BL
    pinMode(GFX_BL, OUTPUT);
    digitalWrite(GFX_BL, HIGH);
#endif

    lv_init();

    screenWidth = gfx->width();
    screenHeight = gfx->height();
    // One draw buffer is 120 rows rather than a whole screen. At RGB565 that
    // is 320 * 120 * 2 bytes, a sensible compromise between smooth rendering
    // and leaving RAM available for audio buffers and the rest of the app.
    bufSize = screenWidth * 120;

    disp_draw_buf1 = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);
    disp_draw_buf2 = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);

    if (!disp_draw_buf1 || !disp_draw_buf2)
    {
        while (1)
        {
            delay(1000);
        }
    }

    lv_disp_draw_buf_init(&draw_buf, disp_draw_buf1, disp_draw_buf2, bufSize);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    // Register touch as an LVGL pointer device. LVGL will now call
    // my_touchpad_read() during lv_timer_handler() in loop().
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    // Create labels/buttons before music_controller_start() writes SD/song
    // status into them. See display_ui_create() for the visible layout.
    display_ui_create();
    // Important order: music_controller_start() may decode/copy wallpaper
    // pixels from SD. It does that now, before audio_player.cpp starts the WAV
    // streaming task, so loading art cannot cause song clicks.
    music_controller_start();
}

// ============================================================================
//  MAIN LOOP
//  Let LVGL process rendering, animations, and input continuously
// ============================================================================
void loop(void)
{
    // Carry playback state onto existing UI widgets; audio feeding stays in
    // its dedicated task.
    music_controller_update();

    // LVGL runs touchscreen callbacks here. For example, tapping pause jumps
    // to pause_button_event_cb() in src/display_ui.cpp.
    // This can take noticeable time when a drawer animation redraws pixels,
    // which is precisely why audio bytes are fed on a different FreeRTOS task.
    lv_timer_handler();
    delay(5);
}
