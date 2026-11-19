#pragma once

#include <Arduino.h>

// Compile-time defaults, overridable per platformio.ini env via build_flags
// (e.g. -D DEFAULT_DPI=300). These only take effect on first boot
// / after an NVS erase -- loadConfig() prefers whatever's already saved in
// Preferences, since these are meant to seed a fresh device, not override one
// a user already configured through the web UI.
#ifndef DEFAULT_DPI
#define DEFAULT_DPI 203
#endif

enum class LogLevel : uint8_t {
  LOG_ERROR = 0,
  LOG_WARN = 1,
  LOG_INFO = 2,
  LOG_DEBUG = 3,
};

const char* logLevelName(LogLevel level);
LogLevel logLevelFromString(const String& raw, LogLevel fallback);

static const int MAX_SAVED_WIFI_NETWORKS = 10;

struct WifiNetwork {
  String ssid;
  String password;
};

struct AppConfig {
  String wifiSsid;
  String wifiPassword;
  // Networks remembered via the web UI's WLAN scan "Connect" button (or the
  // Wi-Fi fields on the Save/setup forms) -- up to MAX_SAVED_WIFI_NETWORKS,
  // oldest evicted first once full (see rememberWifiNetwork() below).
  // Independent of wifiSsid/wifiPassword above, which is just whichever
  // network's credentials are currently active/shown in the form;
  // connectWifi() (wifi_manager.cpp) only ever retries that one. This list
  // exists so a previously-used network can be reconnected to from the web
  // UI without retyping its password -- it is not automatic multi-AP
  // failover.
  WifiNetwork savedWifiNetworks[MAX_SAVED_WIFI_NETWORKS];
  int savedWifiNetworkCount = 0;
  // the stikka-server
  // connection: the server base URL, a one-time pairing code (cleared once enrollment
  // succeeds or is rejected) and the credentials the server issued in return.
  String serverUrl;
  String serverPairingCode;
  String serverNodeId;
  String serverNodeSecret;
  String serverWsUrl;
  uint16_t statusIntervalSec = 30;
  // Web UI on/off (AT+webui enable|disable over serial, see at_interface.cpp).
  bool webUiEnabled = true;
  // HTTP Basic auth password for the web UI (user "admin"). Empty = no
  // protection (fresh device, so first-boot setup works without credentials).
  String adminPassword;
  String printerName = "stikka-esp32";
  // Coordinates only: "auto" (the node scans Wi-Fi and the server geolocates
  // it), "<lat>,<lon>" (fixed, e.g. "47.3769,8.5417") or "" (no location).
  String location = "auto";
  // Optional free text (e.g. "Front desk"), shown next to the printer's name.
  String details;
  // Chosen at runtime via the web UI's first-boot setup wizard (see
  // setupWizardComplete() below and web_ui.cpp's renderWizardPage()) rather
  // than fixed by the firmware build -- empty until the wizard is completed.
  String printerProtocol; // "" | "zpl" | "ql" | "seiko"
  String printerMethod;   // "" | "network" | "serial" | "usb_host"
  String zplTargetHost; // printerMethod=="network" only
  uint16_t zplTargetPort = 9100; // printerMethod=="network" only
  int dpi = DEFAULT_DPI;
  int labelWidth = 55;
  int labelLength = 55;
  bool zplCompressionSupported = false; // printer accepts ^GF :Z64:/:B64: data (printerProtocol=="zpl" only)
  bool debugOutput = true;
  // printerMethod=="serial": a dedicated hardware UART (not the USB/
  // programming port) wired straight to the printer -- distinct from the
  // debug output port (always UART0/the programming port, see logging.cpp)
  // so print traffic and log traffic never share a wire.
  int printerUartTxPin = 32;
  int printerUartRxPin = 33;
  uint32_t printerUartBaud = 9600;
  // printerProtocol=="ql": Brother QL raster protocol knobs. The frontend
  // builds the whole raster byte stream client-side (frontend/src/zpl-image.ts),
  // reading these via the status/capabilities JSON (server_bridge.cpp) instead
  // of a firmware-side rasterizer using them directly. Deliberately generic
  // rather than a specific-model lookup table.
  int qlPrintheadPx = 720;      // 720 = standard family, 1296 = wide family (QL-1050/1060N/11xx)
  int qlInvalidateBytes = 200;  // 200 = most models, 400 = QL-800/810W/820NWB
  bool qlAutoCut = true;
  int qlFeedMarginDots = 35;
  int qlRightMarginDots = 0;    // 0 = auto-center; >0 = distance from the head's right edge
  // printerProtocol=="seiko": Seiko SLP-series thermal raster knobs, same
  // deal as the ql* knobs above -- the frontend builds the whole raster byte
  // stream client-side (frontend/src/zpl-image.ts's
  // imageDataURLToSeikoRasterBase64), reading these via the status/
  // capabilities JSON instead of a firmware-side rasterizer using them
  // directly. Deliberately generic rather than a specific-model lookup table
  // (SLP-650/650SE/740/450/620 differ in max dots/line).
  int seikoMaxDots = 576;   // 576 = SLP-650 hardware max dots/line
  int seikoDensity = 0x00;  // print density, 0x00 = 100%
  int seikoSpeed = 0x02;    // 0x00 = draft, 0x02 = fine (300dpi)
  LogLevel logLevel = LogLevel::LOG_INFO; // verbosity for serial output + web Logs tab
};

extern AppConfig cfg;

// cfg.location helpers. A valid location is "", "auto" or "<lat>,<lon>".
bool locationIsAuto();
bool locationParse(const String& value, double& lat, double& lon);  // "<lat>,<lon>" only
bool locationValid(const String& value);

void loadConfig();
void saveConfig();

// Adds ssid/password to cfg.savedWifiNetworks (or updates the password in
// place if ssid is already saved) and persists immediately. Evicts the
// oldest entry (index 0) once already at MAX_SAVED_WIFI_NETWORKS. No-op if
// ssid is empty.
void rememberWifiNetwork(const String& ssid, const String& password);

// Removes ssid from cfg.savedWifiNetworks if present and persists it.
// No-op if not found.
void forgetWifiNetwork(const String& ssid);

// True once the setup wizard's minimum viable fields are in place (printer
// brand/method chosen, station Wi-Fi entered) -- gates whether web_ui.cpp
// serves the minimal first-boot wizard page or the full settings form.
// Deliberately not gated on the server being configured, so a user can finish the
// wizard and fill in the server later without being bounced back to step 1.
bool setupWizardComplete();

// Dumps the full config + derived settings to the log at LOG_INFO,
// bracketed for readability -- called on boot and after a config save so the
// active settings are always visible in the Logs tab / serial output.
void printRuntimeSettings(const char* reason);
