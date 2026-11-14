#ifdef BLING

#include "bling.h"

#include <Adafruit_GC9A01A.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoPixel.h>
#include <SPI.h>
#include <Wire.h>
#include <math.h>
#include <string.h>

#include "logging.h"

// The GFX library's classic 5x7 font data (a private static copy), so the log
// text can be drawn at 1.5x: a size between GFX's text size 1 (6x8 cell) and
// size 2 (12x16), which are the only built-in scales.
#include "glcdfont.c"

// ---- Pins / sizes, all overridable with -D in platformio.ini. The defaults
// avoid the ESP32-S3 pins used by octal PSRAM (35-37), USB (19/20), the boot
// button (0), the on-board status LED (48) and the strapping pins.
#ifndef BLING_NEOPIXEL_PIN
#define BLING_NEOPIXEL_PIN 15
#endif
#ifndef BLING_NEOPIXEL_COUNT
#define BLING_NEOPIXEL_COUNT 10
#endif
#ifndef BLING_NEOPIXEL_BRIGHTNESS
#define BLING_NEOPIXEL_BRIGHTNESS 40  // 0..255; keep low, 10 pixels at full white draw ~600mA
#endif
#ifndef BLING_TFT_SCK
#define BLING_TFT_SCK 12
#endif
#ifndef BLING_TFT_MOSI
#define BLING_TFT_MOSI 11
#endif
#ifndef BLING_TFT_CS
#define BLING_TFT_CS 10
#endif
#ifndef BLING_TFT_DC
#define BLING_TFT_DC 9
#endif
#ifndef BLING_TFT_RST
#define BLING_TFT_RST 8
#endif
#ifndef BLING_TFT_BL
#define BLING_TFT_BL 7
#endif
#ifndef BLING_I2C_SDA
#define BLING_I2C_SDA 4
#endif
#ifndef BLING_I2C_SCL
#define BLING_I2C_SCL 5
#endif

static const int kPixelCount = BLING_NEOPIXEL_COUNT > 10 ? 10 : BLING_NEOPIXEL_COUNT;
static const int kDisplaySize = 240;
static const int kRowHeight = 13;     // 7px glyph at 1.5x = ~11px, plus a 2px gap
static const int kGlyphAdvance = 9;   // 6px cell at 1.5x
static const int kRowTop = 14;
static const int kRowCount = 16;
static const int kMaxEntries = 24;

static Adafruit_NeoPixel strip(kPixelCount, BLING_NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
static Adafruit_GC9A01A tft(BLING_TFT_CS, BLING_TFT_DC, BLING_TFT_RST);
static GFXcanvas16* canvas = nullptr;  // off-screen buffer -> flicker-free redraws (~112KB, lands in PSRAM)

static bool displayReady = false;
static uint16_t rainbowHue16 = 0;
static unsigned long lastRainbowMs = 0;
static unsigned long lastDisplayMs = 0;
static uint32_t lastDisplayedSeq = 0xFFFFFFFF;

static void i2cScan() {
  String found;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      char buf[8];
      snprintf(buf, sizeof(buf), "0x%02X ", addr);
      found += buf;
    }
  }
  dbgPrint("[bling] i2c (SDA=");
  dbgPrint(BLING_I2C_SDA);
  dbgPrint(", SCL=");
  dbgPrint(BLING_I2C_SCL);
  dbgPrint(") devices: ");
  dbgPrintln(found.isEmpty() ? String("none") : found, LogLevel::LOG_INFO);
}

static uint16_t levelColor(LogLevel level) {
  switch (level) {
    case LogLevel::LOG_ERROR: return 0xF800;  // red
    case LogLevel::LOG_WARN: return 0xFFE0;   // yellow
    case LogLevel::LOG_INFO: return 0xFFFF;   // white
    default: return 0x7BEF;                   // grey
  }
}

// Draws one classic-font glyph at 1.5x: each source pixel covers 1 or 2 target
// pixels alternately (x*3/2), which keeps strokes crisp on the canvas.
static void drawGlyph15(int x, int y, unsigned char c, uint16_t color) {
  for (int col = 0; col < 5; col++) {
    const uint8_t bits = font[c * 5 + col];
    const int x0 = x + (col * 3) / 2;
    const int w = x + ((col + 1) * 3) / 2 - x0;
    for (int row = 0; row < 7; row++) {
      if (!(bits & (1 << row))) continue;
      const int y0 = y + (row * 3) / 2;
      canvas->fillRect(x0, y0, w, y + ((row + 1) * 3) / 2 - y0, color);
    }
  }
}

// How many characters fit on a given display row: the display is round, so
// each row only has as much width as the circle's chord at that height.
static int rowCapacity(int row) {
  const float radius = kDisplaySize / 2.0f;
  const float dy = kRowTop + row * kRowHeight + kRowHeight / 2.0f - radius;
  const float half = sqrtf(fmaxf(0.0f, radius * radius - dy * dy)) - 6.0f;
  return (int)(2.0f * half / kGlyphAdvance);
}

// Lays out entries [first, n) from the top row down, wrapping long lines onto
// further rows. Returns the rows used (may exceed kRowCount); when `draw` is
// set, renders them.
static int layoutEntries(const LogView* view, size_t first, size_t n, bool draw) {
  int row = 0;
  for (size_t i = first; i < n; i++) {
    const char* p = view[i].line;
    size_t remaining = strlen(p);
    if (remaining == 0) {
      row++;  // keep blank lines visible, like the web Logs tab
      continue;
    }
    while (remaining > 0) {
      const int cap = row < kRowCount ? rowCapacity(row) : 1;
      const size_t take = remaining < (size_t)cap ? remaining : (size_t)cap;
      if (draw && row < kRowCount) {
        char buf[40];
        const size_t m = take < sizeof(buf) - 1 ? take : sizeof(buf) - 1;
        memcpy(buf, p, m);
        buf[m] = '\0';
        const uint16_t color = levelColor(view[i].level);
        const int x0 = (int)(kDisplaySize / 2.0f - m * kGlyphAdvance / 2.0f);
        for (size_t k = 0; k < m; k++) {
          drawGlyph15(x0 + (int)k * kGlyphAdvance, kRowTop + row * kRowHeight, (unsigned char)buf[k], color);
        }
      }
      p += take;
      remaining -= take;
      row++;
    }
  }
  return row;
}

// Logs fill the screen from the top; once the screen is full the oldest
// entries scroll off the top.
static void renderLog() {
  canvas->fillScreen(0x0000);

  LogView view[kMaxEntries];
  const size_t n = logRecentEntries(view, kMaxEntries);
  size_t first = 0;
  while (first < n && layoutEntries(view, first, n, false) > kRowCount) first++;
  if (first >= n && n > 0) first = n - 1;  // a single huge entry: show its first rows
  layoutEntries(view, first, n, true);
  tft.drawRGBBitmap(0, 0, canvas->getBuffer(), kDisplaySize, kDisplaySize);
}

static void updateRainbow() {
  const unsigned long now = millis();
  if (now - lastRainbowMs < 20) return;
  lastRainbowMs = now;
  rainbowHue16 += 256;  // full cycle in ~5s
  for (int i = 0; i < kPixelCount; i++) {
    const uint16_t hue = rainbowHue16 + (uint32_t)i * 65536UL / kPixelCount;
    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(hue)));
  }
  strip.show();
}

void blingSetup() {
  strip.begin();
  strip.setBrightness(BLING_NEOPIXEL_BRIGHTNESS);
  strip.clear();
  strip.show();

  Wire.begin(BLING_I2C_SDA, BLING_I2C_SCL);
  i2cScan();

  pinMode(BLING_TFT_BL, OUTPUT);
  digitalWrite(BLING_TFT_BL, HIGH);
  SPI.begin(BLING_TFT_SCK, -1, BLING_TFT_MOSI, BLING_TFT_CS);
  tft.begin(40000000);
  tft.fillScreen(0x0000);
  canvas = new GFXcanvas16(kDisplaySize, kDisplaySize);
  displayReady = canvas != nullptr && canvas->getBuffer() != nullptr;
  dbgPrintln(displayReady ? "[bling] display ready" : "[bling] display buffer alloc failed", LogLevel::LOG_INFO);
}

void blingLoop() {
  updateRainbow();

  if (!displayReady) return;
  const unsigned long now = millis();
  if (now - lastDisplayMs < 200) return;
  const uint32_t seq = logLatestSeq();
  if (seq == lastDisplayedSeq) return;
  lastDisplayMs = now;
  lastDisplayedSeq = seq;
  renderLog();
}

#endif  // BLING
