#pragma once

// Registers routes (config page, logs page, captive-portal redirects) and
// starts the HTTP server. Call once from setup(), after wifiManagerSetup().
void webUiSetup();

// Services pending HTTP requests. Call once per loop() iteration.
void webUiLoop();

// Starts/stops the HTTP server at runtime (AT+webui enable|disable). The
// persisted default lives in cfg.webUiEnabled and is applied by webUiSetup().
void webUiSetEnabled(bool enabled);

// Printer type lookup ("<protocol>_<method>" values such as "zpl_network"),
// shared with the AT interface so both offer the same choices.
#include <Arduino.h>
bool printerTypeFromValue(const String& value, String& protocolOut, String& methodOut);
size_t printerTypeCount();
const char* printerTypeValueAt(size_t i);
