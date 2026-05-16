// ============================================================================
//  LIBRARIES
//  Display driver, UI framework, GPIO expander, and capacitive touch driver
// ============================================================================
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include "TCA9554.h"
#include "TouchDrvFT6X36.hpp"

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

Arduino_DataBus *bus = new Arduino_ESP32SPI(
    LCD_DC /* DC */, LCD_CS /* CS */, SPI_SCLK /* SCK */, SPI_MOSI /* MOSI */, SPI_MISO /* MISO */);
Arduino_GFX *gfx = new Arduino_ST7796(
    bus, LCD_RST /* RST */, 0 /* rotation */, true /* IPS */, LCD_HOR_RES, LCD_VER_RES);

// ============================================================================
//  LVGL STATE
//  Buffers, display driver state, and UI objects shared across the program
// ============================================================================
uint32_t screenWidth;
uint32_t screenHeight;
uint32_t bufSize;
lv_disp_draw_buf_t draw_buf;
lv_color_t *disp_draw_buf1;
lv_color_t *disp_draw_buf2;
lv_disp_drv_t disp_drv;
lv_obj_t *status_label;

// ============================================================================
//  DISPLAY HARDWARE HELPERS
//  Panel reset and the bridge from LVGL pixels to the physical LCD
// ============================================================================
void lcd_reset(void)
{
    TCA.write1(1, 1);
    delay(10);
    TCA.write1(1, 0);
    delay(10);
    TCA.write1(1, 1);
    delay(200);
}

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
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
        data->state = LV_INDEV_STATE_PR;
        data->point.x = x[0];
        data->point.y = y[0];
    }
    else
    {
        data->state = LV_INDEV_STATE_REL;
    }
}

// ============================================================================
//  UI EVENTS
//  Callback functions that define what widgets do when the user interacts
// ============================================================================
void button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED)
    {
        lv_label_set_text(status_label, "Button pressed!");
    }
}

// ============================================================================
//  UI CONSTRUCTION
//  Build and arrange the visible LVGL widgets for this screen
// ============================================================================
void create_ui(void)
{
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x101820), LV_PART_MAIN);

    status_label = lv_label_create(lv_scr_act());
    lv_label_set_text(status_label, "LVGL is alive");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 36);

    //Example button
    lv_obj_t *button = lv_btn_create(lv_scr_act());
    lv_obj_set_size(button, 180, 70);
    lv_obj_align(button, LV_ALIGN_CENTER, 0, 10);
    lv_obj_add_event_cb(button, button_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *button_label = lv_label_create(button);
    lv_label_set_text(button_label, "Tap me");
    lv_obj_center(button_label);

    //My test button
    lv_obj_t *button2 = lv_btn_create(lv_scr_act());
    lv_obj_set_size(button2, 180, 70);
    lv_obj_align(button2, LV_ALIGN_CENTER, 0, 90);
    lv_obj_add_event_cb(button2, button_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *button_label2 = lv_label_create(button2);
    lv_label_set_text(button_label2, "Tap me as well");
    lv_obj_center(button_label2);
}

// ============================================================================
//  STARTUP
//  Bring up the board, initialize LVGL, register drivers, and create the UI
// ============================================================================
void setup(void)
{
    Serial.begin(115200);

    Wire.begin(I2C_SDA, I2C_SCL);
    TCA.begin();
    TCA.pinMode1(1, OUTPUT);
    lcd_reset();

    if (!touch.begin(Wire, FT6X36_SLAVE_ADDRESS))
    {
        Serial.println("Failed to find FT6X36 - check your wiring!");
        while (1)
        {
            delay(1000);
        }
    }

    if (!gfx->begin())
    {
        Serial.println("gfx->begin() failed!");
    }
    gfx->fillScreen(BLACK);

#ifdef GFX_BL
    pinMode(GFX_BL, OUTPUT);
    digitalWrite(GFX_BL, HIGH);
#endif

    lv_init();

    screenWidth = gfx->width();
    screenHeight = gfx->height();
    bufSize = screenWidth * 120;

    disp_draw_buf1 = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);
    disp_draw_buf2 = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);

    if (!disp_draw_buf1 || !disp_draw_buf2)
    {
        Serial.println("LVGL display buffer allocation failed!");
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

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    create_ui();
    Serial.println("Minimal LVGL UI ready");
}

// ============================================================================
//  MAIN LOOP
//  Let LVGL process rendering, animations, and input continuously
// ============================================================================
void loop(void)
{
    lv_timer_handler();
    delay(5);
}
