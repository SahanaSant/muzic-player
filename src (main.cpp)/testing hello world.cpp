#include <Arduino_GFX_Library.h>
#include "TCA9554.h"

#define GFX_BL 6  // default backlight pin, you may replace DF_GFX_BL to actual backlight pin

#define SPI_MISO 2 //spi pins for sending image data
#define SPI_MOSI 1
#define SPI_SCLK 5

#define LCD_CS -1 
#define LCD_DC 3
#define LCD_RST -1
#define LCD_HOR_RES 320
#define LCD_VER_RES 480

#define I2C_SDA 8 
#define I2C_SCL 7

TCA9554 TCA(0x20); //i2c chip helper object


//spi and bus connection, configuration
Arduino_DataBus* bus = new Arduino_ESP32SPI(LCD_DC /* DC */, LCD_CS /* CS */, SPI_SCLK /* SCK */, SPI_MOSI /* MOSI */, SPI_MISO /* MISO */);
Arduino_GFX* gfx = new Arduino_ST7796(
  bus, LCD_RST /* RST */, 0 /* rotation */, true, LCD_HOR_RES, LCD_VER_RES);

//reset the lcd, clear anything that was on it before. 
void lcd_reset(void) {
  TCA.write1(1, 1);
  delay(10);
  TCA.write1(1, 0);
  delay(10);
  TCA.write1(1, 1);
  delay(200);
}

void setup(void) {

  Serial.begin(115200);

  Wire.begin(I2C_SDA, I2C_SCL);
  TCA.begin();
  TCA.pinMode1(1,OUTPUT);   //initialization. 

  lcd_reset();
  // Serial.setDebugOutput(true);
  // while(!Serial);
  Serial.println("Arduino_GFX Hello World example");

  // Init Display
  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
  }

  gfx->fillScreen(BLACK); //Fill the screen with black. 

#ifdef GFX_BL
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
#endif //Thirs turns on the backlight

  gfx->setCursor(10, 10);
  gfx->setTextColor(RED);
  gfx->println("Hello World!"); //first hello world you see in the top left corner

  delay(2000);  // 2 seconds
}

void loop() {
  gfx->setCursor(random(gfx->width()), random(gfx->height())); //choose a random x,y point somewhere
  gfx->setTextColor(random(0xffff), random(0xffff));
  gfx->setTextSize(random(6) /* x scale */, random(6) /* y scale */, random(2) /* pixel_margin */);
  gfx->println("Hello World!");

  delay(1000);  // 1 second
}
