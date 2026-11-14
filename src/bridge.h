#pragma once

#include <Arduino.h>

// One-line human-readable link state for the web UI
// ("paired as X, connected" / "enrolling..." / last error).
String serverBridgeStatus();

bool bridgeIsConnected();

// Call once from setup().
void bridgeSetup();

// Enrolls / connects (retrying), services the WebSocket and the periodic
// status push. Call once per loop() iteration.
void bridgeLoop();

void publishStatus(const char* phase = "ready", const char* lastError = "");

// Applies changed name/server settings without touching Wi-Fi: just pushes a
// fresh status (and re-enrolls if the server URL / pairing code changed).
void bridgeReconnect();
