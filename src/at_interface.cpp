#include "at_interface.h"

#include <WiFi.h>
#include <vector>

#include "bridge.h"
#include "config.h"
#include "logging.h"
#include "status_led.h"
#include "targets/target.h"
#include "web_ui.h"
#include "wifi_manager.h"

#ifndef BOOT_BUTTON_PIN
#define BOOT_BUTTON_PIN 0  // BOOT button on the ESP32-S3 DevKitC-1
#endif
static const uint32_t CONFIG_HOLD_MS = 10000;
static const size_t LINE_MAX_LEN = 384;

// What has to be re-applied once a command (or the onboarding) is done.
enum ApplyFlag : uint8_t {
  A_WIFI = 1,
  A_NAME = 2,
  A_SERVER = 4,
  A_PRINTER = 8,
  A_RESTART = 16,  // only takes full effect after AT+reboot
};

enum class Kind : uint8_t { Str, Int, U16, U32, Bool };

struct Setting {
  const char* name;
  Kind kind;
  void* ptr;
  long min;
  long max;
  bool secret;
  uint8_t apply;
  const char* help;
};

static const Setting kSettings[] = {
    {"wifi.ssid", Kind::Str, &cfg.wifiSsid, 0, 32, false, A_WIFI, "Wi-Fi network name"},
    {"wifi.password", Kind::Str, &cfg.wifiPassword, 0, 64, true, A_WIFI, "Wi-Fi password (\"\" = open network)"},
    {"server.url", Kind::Str, &cfg.serverUrl, 0, 200, false, A_SERVER, "stikka-server base URL"},
    {"server.code", Kind::Str, &cfg.serverPairingCode, 0, 32, false, A_SERVER, "one-time pairing code (pairs / re-pairs)"},
    {"name", Kind::Str, &cfg.printerName, 1, 63, false, A_NAME, "printer / node name (also the mDNS hostname)"},
    {"location", Kind::Str, &cfg.location, 0, 100, false, A_SERVER, "free-form details shown next to the printer"},
    {"admin.password", Kind::Str, &cfg.adminPassword, 0, 64, true, 0, "web UI password, user \"admin\" (\"\" = no password)"},
    {"target.host", Kind::Str, &cfg.zplTargetHost, 0, 100, false, A_PRINTER, "network printer host (zpl_network only)"},
    {"target.port", Kind::U16, &cfg.zplTargetPort, 1, 65535, false, A_PRINTER, "network printer port (default 9100)"},
    {"dpi", Kind::Int, &cfg.dpi, 72, 1200, false, A_SERVER, "printer resolution in dpi"},
    {"label.width", Kind::Int, &cfg.labelWidth, 1, 1000, false, A_SERVER, "label width in mm"},
    {"label.length", Kind::Int, &cfg.labelLength, 0, 5000, false, A_SERVER, "label length in mm (0 = endless)"},
    {"zpl.compression", Kind::Bool, &cfg.zplCompressionSupported, 0, 1, false, A_SERVER, "printer accepts :Z64:/:B64: graphics (enable|disable)"},
    {"ql.printhead", Kind::Int, &cfg.qlPrintheadPx, 720, 1296, false, A_SERVER, "Brother QL head width in px: 720 or 1296"},
    {"ql.invalidate", Kind::Int, &cfg.qlInvalidateBytes, 1, 2000, false, A_SERVER, "Brother QL invalidate bytes (200, 400 on QL-8xx)"},
    {"ql.autocut", Kind::Bool, &cfg.qlAutoCut, 0, 1, false, A_SERVER, "Brother QL auto cut (enable|disable)"},
    {"ql.feed", Kind::Int, &cfg.qlFeedMarginDots, 0, 1000, false, A_SERVER, "Brother QL feed margin in dots"},
    {"ql.rightmargin", Kind::Int, &cfg.qlRightMarginDots, 0, 1296, false, A_SERVER, "Brother QL right margin in dots (0 = auto-center)"},
    {"seiko.maxdots", Kind::Int, &cfg.seikoMaxDots, 1, 2000, false, A_SERVER, "Seiko SLP max dots per line"},
    {"seiko.density", Kind::Int, &cfg.seikoDensity, 0, 255, false, A_SERVER, "Seiko SLP print density (0 = 100%)"},
    {"seiko.speed", Kind::Int, &cfg.seikoSpeed, 0, 255, false, A_SERVER, "Seiko SLP speed (0 = draft, 2 = fine)"},
    {"uart.tx", Kind::Int, &cfg.printerUartTxPin, 0, 48, false, A_PRINTER, "printer UART TX pin (serial method)"},
    {"uart.rx", Kind::Int, &cfg.printerUartRxPin, 0, 48, false, A_PRINTER, "printer UART RX pin (serial method)"},
    {"uart.baud", Kind::U32, &cfg.printerUartBaud, 300, 2000000, false, A_PRINTER, "printer UART baud rate (serial method)"},
    {"status.interval", Kind::U16, &cfg.statusIntervalSec, 1, 3600, false, 0, "status push interval in seconds"},
    {"debug", Kind::Bool, &cfg.debugOutput, 0, 1, false, 0, "serial log output after leaving config mode (enable|disable)"},
};
static const size_t kSettingCount = sizeof(kSettings) / sizeof(kSettings[0]);

static bool active = false;
static uint8_t pendingApply = 0;
static String lineBuf;
static uint32_t pressStartMs = 0;
static bool lastLineWasCR = false;

// Onboarding state
static bool onboarding = false;
static std::vector<const char*> obKeys;
static size_t obIdx = 0;

// ---------------------------------------------------------------- helpers

static void say(const String& s) { Serial.print(s); }
static void sayln(const String& s = "") {
  Serial.print(s);
  Serial.print("\r\n");
}
static void ok() { sayln("OK"); }
static void fail(const String& msg) { sayln("ERROR: " + msg); }

static const Setting* findSetting(const String& name) {
  for (size_t i = 0; i < kSettingCount; i++) {
    if (name.equalsIgnoreCase(kSettings[i].name)) return &kSettings[i];
  }
  return nullptr;
}

static String valueToString(const Setting& s, bool mask) {
  switch (s.kind) {
    case Kind::Str: {
      const String& v = *(String*)s.ptr;
      if (mask && s.secret) return v.isEmpty() ? "<empty>" : "<set>";
      return v;
    }
    case Kind::Int: return String(*(int*)s.ptr);
    case Kind::U16: return String(*(uint16_t*)s.ptr);
    case Kind::U32: return String(*(uint32_t*)s.ptr);
    case Kind::Bool: return *(bool*)s.ptr ? "enabled" : "disabled";
  }
  return "";
}

static bool parseBool(String v, bool& out) {
  v.trim();
  v.toLowerCase();
  if (v == "enable" || v == "enabled" || v == "on" || v == "true" || v == "yes" || v == "1") { out = true; return true; }
  if (v == "disable" || v == "disabled" || v == "off" || v == "false" || v == "no" || v == "0") { out = false; return true; }
  return false;
}

static String unquote(String v) {
  v.trim();
  if (v.length() >= 2 && v[0] == '"' && v[v.length() - 1] == '"') return v.substring(1, v.length() - 1);
  return v;
}

static bool parseLong(const String& v, long& out) {
  String t = v;
  t.trim();
  if (t.isEmpty()) return false;
  char* end = nullptr;
  out = strtol(t.c_str(), &end, 0);  // base 0: accepts 0x.. for seiko values too
  return end != nullptr && *end == '\0';
}

// Validates and stores one value. Does not persist.
static bool setValue(const Setting& s, const String& rawArg, String& err) {
  switch (s.kind) {
    case Kind::Str: {
      String v = unquote(rawArg);
      if ((long)v.length() < s.min || (long)v.length() > s.max) {
        err = String("length must be ") + s.min + ".." + s.max;
        return false;
      }
      *(String*)s.ptr = v;
      return true;
    }
    case Kind::Bool: {
      bool b;
      if (!parseBool(rawArg, b)) { err = "expected enable|disable"; return false; }
      *(bool*)s.ptr = b;
      return true;
    }
    default: {
      long n;
      if (!parseLong(rawArg, n)) { err = "expected a number"; return false; }
      if (n < s.min || n > s.max) { err = String("must be ") + s.min + ".." + s.max; return false; }
      if (String(s.name) == "ql.printhead" && n != 720 && n != 1296) { err = "must be 720 or 1296"; return false; }
      if (s.kind == Kind::Int) *(int*)s.ptr = (int)n;
      else if (s.kind == Kind::U16) *(uint16_t*)s.ptr = (uint16_t)n;
      else *(uint32_t*)s.ptr = (uint32_t)n;
      return true;
    }
  }
}

static String currentTypeValue() {
  if (cfg.printerProtocol.isEmpty() || cfg.printerMethod.isEmpty()) return "";
  return cfg.printerProtocol + "_" + cfg.printerMethod;
}

static String typeOptionsText() {
  String t;
  for (size_t i = 0; i < printerTypeCount(); i++) {
    if (i) t += ", ";
    t += String(i + 1) + "=" + printerTypeValueAt(i);
  }
  return t;
}

static bool setType(String v, String& err) {
  v = unquote(v);
  v.toLowerCase();
  long n;
  if (parseLong(v, n) && n >= 1 && (size_t)n <= printerTypeCount()) v = printerTypeValueAt((size_t)n - 1);
  String protocol, method;
  if (!printerTypeFromValue(v, protocol, method)) {
    err = "unknown type; options: " + typeOptionsText();
    return false;
  }
  const bool changed = protocol != cfg.printerProtocol || method != cfg.printerMethod;
  cfg.printerProtocol = protocol;
  cfg.printerMethod = method;
  // Same protocol-appropriate DPI pre-fill as the web setup wizard.
  if (cfg.dpi == DEFAULT_DPI && (protocol == "ql" || protocol == "seiko")) cfg.dpi = 300;
  pendingApply |= A_PRINTER | A_SERVER | (changed ? A_RESTART : 0);
  return true;
}

static void applyPending() {
  const uint8_t f = pendingApply;
  pendingApply = 0;
  if (f == 0) return;
  if (f & A_WIFI) {
    rememberWifiNetwork(cfg.wifiSsid, cfg.wifiPassword);
    wifiForceDisconnect();
    wifiRetryNow();
  }
  if (f & A_NAME) wifiRestartMdns();
  if (f & A_PRINTER) targetSetup();
  if (f & (A_SERVER | A_NAME)) bridgeReconnect();
  if (f & A_RESTART) sayln("NOTE: printer type changed -- run AT+reboot for it to take full effect.");
}

// --------------------------------------------------------------- commands

static void cmdHelp() {
  sayln("Stikka AT config interface. Syntax: AT+<command> [value]");
  sayln("A command without a value shows the current setting. Use \"\" to clear a text value.");
  sayln("");
  sayln("General:");
  sayln("  AT+help                     this list");
  sayln("  AT+status                   show all settings and link state");
  sayln("  AT+onboarding               guided setup of every setting (works on configured nodes)");
  sayln("  AT+webui [enable|disable]   web UI on/off (default on)");
  sayln("  AT+type [value|number]      printer type: " + typeOptionsText());
  sayln("  AT+loglevel [error|warn|info|debug]   log verbosity");
  sayln("  AT+wifi.list                saved Wi-Fi networks");
  sayln("  AT+wifi.forget <ssid>       remove a saved Wi-Fi network");
  sayln("  AT+wifi.connect             reconnect Wi-Fi with the current credentials");
  sayln("  AT+server.unpair            forget the server pairing (node id/secret)");
  sayln("  AT+reboot                   restart the node");
  sayln("  AT+exit                     leave config mode");
  sayln("");
  sayln("Settings (AT+<name> [value]):");
  for (size_t i = 0; i < kSettingCount; i++) {
    String line = String("  AT+") + kSettings[i].name;
    while (line.length() < 28) line += ' ';
    sayln(line + kSettings[i].help);
  }
}

static void cmdStatus() {
  sayln("Wi-Fi:      " + String(wifiIsConnected() ? ("connected, " + WiFi.localIP().toString()) : String("not connected")));
  sayln("Server:     " + serverBridgeStatus());
  sayln("webui:      " + String(cfg.webUiEnabled ? "enabled" : "disabled"));
  sayln("type:       " + (currentTypeValue().isEmpty() ? String("<not set>") : currentTypeValue()));
  sayln("loglevel:   " + String(logLevelName(cfg.logLevel)));
  for (size_t i = 0; i < kSettingCount; i++) {
    String key = String(kSettings[i].name) + ":";
    while (key.length() < 16) key += ' ';
    sayln(key + valueToString(kSettings[i], true));
  }
}

static void startOnboarding();

static void handleCommand(const String& cmd, const String& arg, bool hasArg) {
  if (cmd == "help" || cmd == "?") { cmdHelp(); ok(); return; }
  if (cmd == "status") { cmdStatus(); ok(); return; }
  if (cmd == "onboarding") { startOnboarding(); return; }
  if (cmd == "exit") {
    sayln("Leaving config mode.");
    ok();
    Serial.flush();
    active = false;
    setStatusLedConfigMode(false);
    logSerialRelease();
    dbgPrintln("[at] config mode left", LogLevel::LOG_INFO);
    return;
  }
  if (cmd == "reboot") {
    ok();
    Serial.flush();
    delay(200);
    ESP.restart();
    return;
  }
  if (cmd == "webui") {
    if (!hasArg) { sayln(String("webui=") + (cfg.webUiEnabled ? "enabled" : "disabled")); ok(); return; }
    bool b;
    if (!parseBool(arg, b)) { fail("usage: AT+webui enable|disable"); return; }
    cfg.webUiEnabled = b;
    saveConfig();
    webUiSetEnabled(b);
    sayln(String("webui=") + (b ? "enabled" : "disabled"));
    ok();
    return;
  }
  if (cmd == "type") {
    if (!hasArg) {
      sayln("type=" + (currentTypeValue().isEmpty() ? String("<not set>") : currentTypeValue()));
      sayln("options: " + typeOptionsText());
      ok();
      return;
    }
    String err;
    if (!setType(arg, err)) { fail(err); return; }
    saveConfig();
    sayln("type=" + currentTypeValue());
    applyPending();
    ok();
    return;
  }
  if (cmd == "loglevel") {
    if (!hasArg) { sayln(String("loglevel=") + logLevelName(cfg.logLevel)); ok(); return; }
    String v = arg;
    v.trim();
    if (!(v.equalsIgnoreCase("error") || v.equalsIgnoreCase("warn") || v.equalsIgnoreCase("info") || v.equalsIgnoreCase("debug"))) {
      fail("usage: AT+loglevel error|warn|info|debug");
      return;
    }
    cfg.logLevel = logLevelFromString(v, cfg.logLevel);
    saveConfig();
    sayln(String("loglevel=") + logLevelName(cfg.logLevel));
    ok();
    return;
  }
  if (cmd == "wifi.list") {
    if (cfg.savedWifiNetworkCount == 0) sayln("(no saved networks)");
    for (int i = 0; i < cfg.savedWifiNetworkCount; i++) {
      sayln(String(i + 1) + ". " + cfg.savedWifiNetworks[i].ssid + (cfg.savedWifiNetworks[i].ssid == cfg.wifiSsid ? "  (active)" : ""));
    }
    ok();
    return;
  }
  if (cmd == "wifi.forget") {
    String ssid = unquote(arg);
    if (ssid.isEmpty()) { fail("usage: AT+wifi.forget <ssid>"); return; }
    forgetWifiNetwork(ssid);
    ok();
    return;
  }
  if (cmd == "wifi.connect") {
    pendingApply |= A_WIFI;
    applyPending();
    ok();
    return;
  }
  if (cmd == "server.unpair") {
    cfg.serverNodeId = "";
    cfg.serverNodeSecret = "";
    cfg.serverWsUrl = "";
    cfg.serverPairingCode = "";
    saveConfig();
    bridgeReconnect();
    ok();
    return;
  }

  const Setting* s = findSetting(cmd);
  if (!s) { fail("unknown command '" + cmd + "' (try AT+help)"); return; }
  if (!hasArg) {
    sayln(String(s->name) + "=" + valueToString(*s, true));
    ok();
    return;
  }
  String err;
  if (!setValue(*s, arg, err)) { fail(err); return; }
  pendingApply |= s->apply;
  saveConfig();
  sayln(String(s->name) + "=" + valueToString(*s, true));
  applyPending();
  ok();
}

// ------------------------------------------------------------- onboarding

static void buildOnboardingTail() {
  obKeys.resize(7);  // fixed head: see startOnboarding()
  if (cfg.printerMethod == "network") {
    obKeys.push_back("target.host");
    obKeys.push_back("target.port");
  }
  obKeys.push_back("dpi");
  obKeys.push_back("label.width");
  obKeys.push_back("label.length");
  if (cfg.printerProtocol == "zpl") obKeys.push_back("zpl.compression");
  if (cfg.printerProtocol == "ql") {
    obKeys.push_back("ql.printhead");
    obKeys.push_back("ql.invalidate");
    obKeys.push_back("ql.autocut");
    obKeys.push_back("ql.feed");
    obKeys.push_back("ql.rightmargin");
  }
  if (cfg.printerProtocol == "seiko") {
    obKeys.push_back("seiko.maxdots");
    obKeys.push_back("seiko.density");
    obKeys.push_back("seiko.speed");
  }
  obKeys.push_back("admin.password");
}

static void promptOnboarding() {
  const String key = obKeys[obIdx];
  String head = String("[") + (obIdx + 1) + "/" + obKeys.size() + "] ";
  if (key == "type") {
    sayln(head + "Printer type -- " + typeOptionsText());
    say("  current: " + (currentTypeValue().isEmpty() ? String("<not set>") : currentTypeValue()) + "  > ");
    return;
  }
  const Setting* s = findSetting(key);
  sayln(head + s->help);
  say(String("  ") + s->name + " [" + valueToString(*s, true) + "]  > ");
}

static void finishOnboarding(bool cancelled) {
  onboarding = false;
  saveConfig();
  pendingApply |= A_WIFI | A_NAME | A_SERVER | A_PRINTER;
  applyPending();
  sayln("");
  sayln(cancelled ? "Onboarding cancelled; answers given so far were kept." : "Onboarding complete.");
  cmdStatus();
  ok();
}

static void startOnboarding() {
  onboarding = true;
  obKeys = {"wifi.ssid", "wifi.password", "server.url", "server.code", "name", "location", "type"};
  buildOnboardingTail();
  obIdx = 0;
  sayln("Onboarding: Enter keeps the current value, \"\" clears a text value, AT+cancel aborts.");
  promptOnboarding();
}

static void handleOnboardingLine(const String& line) {
  String t = line;
  t.trim();
  if (t.equalsIgnoreCase("AT+cancel")) { finishOnboarding(true); return; }

  const String key = obKeys[obIdx];
  if (!t.isEmpty()) {
    String err;
    bool good;
    if (key == "type") {
      good = setType(t, err);
    } else {
      const Setting* s = findSetting(key);
      good = setValue(*s, t, err);
      if (good) pendingApply |= s->apply;
    }
    if (!good) {
      sayln("  invalid: " + err);
      promptOnboarding();
      return;
    }
    saveConfig();
  } else if (key == "type" && currentTypeValue().isEmpty()) {
    sayln("  a printer type is required");
    promptOnboarding();
    return;
  }
  if (key == "type") buildOnboardingTail();
  obIdx++;
  if (obIdx >= obKeys.size()) {
    finishOnboarding(false);
    return;
  }
  promptOnboarding();
}

// ------------------------------------------------------------ line intake

static void handleLine(String line) {
  if (onboarding) { handleOnboardingLine(line); return; }
  line.trim();
  if (line.isEmpty()) return;
  if (!(line.length() >= 3 && line.substring(0, 3).equalsIgnoreCase("AT+"))) {
    if (line.equalsIgnoreCase("AT")) { ok(); return; }
    fail("commands start with AT+ (try AT+help)");
    return;
  }
  String body = line.substring(3);
  int split = -1;
  for (size_t i = 0; i < body.length(); i++) {
    if (body[i] == ' ' || body[i] == '=') { split = (int)i; break; }
  }
  String cmd = split < 0 ? body : body.substring(0, split);
  String arg = split < 0 ? String("") : body.substring(split + 1);
  cmd.toLowerCase();
  arg.trim();
  handleCommand(cmd, arg, split >= 0 && !arg.isEmpty());
}

static void enterConfigMode() {
  active = true;
  onboarding = false;
  lineBuf = "";
  logSerialAcquireForAt();
  setStatusLedConfigMode(true);
  dbgPrintln("[at] config mode entered (boot button held)", LogLevel::LOG_INFO);
  sayln("");
  sayln("=== Stikka config mode ===");
  sayln("Type AT+help for the command list, AT+exit to leave.");
  ok();
}

void atInterfaceSetup() {
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  pressStartMs = 0;
}

bool atInterfaceActive() { return active; }

void atInterfaceLoop() {
  if (!active) {
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
      const uint32_t now = millis();
      if (pressStartMs == 0) pressStartMs = now ? now : 1;
      if (now - pressStartMs >= CONFIG_HOLD_MS) {
        pressStartMs = 0;
        enterConfigMode();
      }
    } else {
      pressStartMs = 0;
    }
    return;
  }

  while (Serial.available() > 0 && active) {
    const char c = (char)Serial.read();
    if (c == '\n' && lastLineWasCR) { lastLineWasCR = false; continue; }  // CRLF pair
    lastLineWasCR = (c == '\r');
    if (c == '\r' || c == '\n') {
      sayln("");
      String line = lineBuf;
      lineBuf = "";
      handleLine(line);
    } else if (c == 0x08 || c == 0x7f) {
      if (lineBuf.length() > 0) {
        lineBuf.remove(lineBuf.length() - 1);
        say("\b \b");
      }
    } else if (c >= 0x20 && lineBuf.length() < LINE_MAX_LEN) {
      lineBuf += c;
      Serial.write(c);
    }
  }
}
