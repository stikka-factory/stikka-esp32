// Standalone GC9A01 screen test (env esp32-s3-devkitc-1-screentest). Not part
// of the firmware: cycles through fills, a round-edge ring, a color wheel and
// a text/counter page so wiring, orientation and colors can be checked.
#include <Adafruit_GC9A01A.h>
#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <SPI.h>
#include <math.h>

#ifndef BLING_TFT_SCK
#define BLING_TFT_SCK 12
#define BLING_TFT_MOSI 11
#define BLING_TFT_CS 10
#define BLING_TFT_DC 9
#define BLING_TFT_RST 8
#define BLING_TFT_BL 7
#endif

static Adafruit_GC9A01A tft(BLING_TFT_CS, BLING_TFT_DC, BLING_TFT_RST);

static void label(const char* text, uint16_t color) {
  tft.setTextSize(2);
  tft.setTextColor(color);
  int16_t x, y;
  uint16_t w, h;
  tft.getTextBounds(text, 0, 0, &x, &y, &w, &h);
  tft.setCursor(120 - w / 2, 112);
  tft.print(text);
}

static void fill(uint16_t color, const char* name, uint16_t textColor) {
  tft.fillScreen(color);
  label(name, textColor);
  Serial.println(name);
  delay(1200);
}

static void rings() {
  tft.fillScreen(0);
  tft.drawCircle(120, 120, 119, 0xFFFF);  // outermost visible ring: shows clipping
  tft.drawCircle(120, 120, 100, 0xF800);
  tft.drawCircle(120, 120, 80, 0x07E0);
  tft.drawCircle(120, 120, 60, 0x001F);
  tft.drawCircle(120, 120, 40, 0xFFE0);
  tft.drawFastHLine(0, 120, 240, 0x7BEF);
  tft.drawFastVLine(120, 0, 240, 0x7BEF);
  tft.setTextSize(1);
  tft.setTextColor(0xFFFF);
  tft.setCursor(124, 4);
  tft.print("N");
  tft.setCursor(226, 116);
  tft.print("E");
  Serial.println("rings");
  delay(2000);
}

static void colorWheel() {
  for (int y = 0; y < 240; y++) {
    for (int x = 0; x < 240; x++) {
      const float dx = x - 119.5f, dy = y - 119.5f;
      const float r = sqrtf(dx * dx + dy * dy);
      if (r > 120) continue;
      const float a = atan2f(dy, dx) + (float)M_PI;  // 0..2pi
      const float h = a / (2.0f * (float)M_PI) * 6.0f;
      const int i = (int)h;
      const float f = h - i;
      const float s = r / 120.0f;
      float R, G, B;
      const float p = 1 - s, q = 1 - s * f, t = 1 - s * (1 - f);
      switch (i % 6) {
        case 0: R = 1; G = t; B = p; break;
        case 1: R = q; G = 1; B = p; break;
        case 2: R = p; G = 1; B = t; break;
        case 3: R = p; G = q; B = 1; break;
        case 4: R = t; G = p; B = 1; break;
        default: R = 1; G = p; B = q; break;
      }
      tft.drawPixel(x, y, tft.color565((uint8_t)(R * 255), (uint8_t)(G * 255), (uint8_t)(B * 255)));
    }
  }
  Serial.println("color wheel");
  delay(2000);
}

static void counterPage() {
  GFXcanvas16 canvas(240, 240);
  if (canvas.getBuffer() == nullptr) {
    Serial.println("canvas alloc failed");
    return;
  }
  Serial.println("counter (canvas, flicker-free)");
  for (int n = 0; n <= 60; n++) {
    canvas.fillScreen(0);
    canvas.drawCircle(120, 120, 118, 0x07FF);
    canvas.setTextColor(0xFFFF);
    canvas.setTextSize(2);
    canvas.setCursor(48, 70);
    canvas.print("SCREEN TEST");
    canvas.setTextSize(5);
    canvas.setCursor(n < 10 ? 100 : 80, 105);
    canvas.print(n);
    canvas.setTextSize(1);
    canvas.setTextColor(0x7BEF);
    canvas.setCursor(66, 170);
    canvas.print("GC9A01 240x240 round");
    const float a = n * 0.2f;
    canvas.fillCircle(120 + (int)(100 * cosf(a)), 120 + (int)(100 * sinf(a)), 6, 0xF81F);
    tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 240, 240);
    delay(40);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nStikka GC9A01 screen test");
  pinMode(BLING_TFT_BL, OUTPUT);
  digitalWrite(BLING_TFT_BL, HIGH);
  SPI.begin(BLING_TFT_SCK, -1, BLING_TFT_MOSI, BLING_TFT_CS);
  tft.begin(40000000);
}

void loop() {
  fill(0xF800, "RED", 0xFFFF);
  fill(0x07E0, "GREEN", 0x0000);
  fill(0x001F, "BLUE", 0xFFFF);
  fill(0xFFFF, "WHITE", 0x0000);
  fill(0x0000, "BLACK", 0xFFFF);
  rings();
  colorWheel();
  counterPage();
}
