/*
  Nova Light — ESP32-WROOM-32 local smart-light controller

  Required Arduino board package: Espressif ESP32
  External libraries: none
  Web interface: dashboard_html.h, generated from ../dashboard.html
*/
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>
#include <pgmspace.h>
#include "dashboard_html.h"
#include "secrets.h"

// Edit these two values before uploading. The ESP32 joins your home Wi-Fi.

// Common relay modules are active LOW. Change to HIGH if your module is active HIGH.
#define RELAY_ACTIVE_LEVEL LOW
constexpr uint8_t RELAY_PIN = 25;
constexpr uint8_t RELAY_ON_LEVEL = RELAY_ACTIVE_LEVEL;
constexpr uint8_t RELAY_OFF_LEVEL = (RELAY_ACTIVE_LEVEL == HIGH) ? LOW : HIGH;

constexpr uint16_t DEFAULT_ON_MINUTE = 18 * 60 + 40;  // 6:40 PM
constexpr uint16_t DEFAULT_OFF_MINUTE = 6 * 60 + 40; // 6:40 AM
constexpr uint8_t HISTORY_LIMIT = 10;
constexpr uint32_t USAGE_SAVE_INTERVAL_MS = 60000;
constexpr time_t VALID_CLOCK_EPOCH = 1704067200; // 2024-01-01, used to detect missing NTP time

WebServer server(80);
Preferences preferences;

struct HistoryEntry {
  String label;
  String timestamp;
};

HistoryEntry historyEntries[HISTORY_LIMIT];
uint8_t historyCount = 0;
bool relayIsOn = false;
bool scheduleEnabled = false;
uint16_t onMinute = DEFAULT_ON_MINUTE;
uint16_t offMinute = DEFAULT_OFF_MINUTE;
uint32_t usageDayKey = 0;
uint32_t dailyUsageSeconds = 0;
uint32_t lastUsageTickMs = 0;
uint32_t lastUsageSaveMs = 0;
int64_t lastScheduleMinute = -1;
bool scheduleCheckPending = true;
uint32_t lastReconnectAttemptMs = 0;

struct JsonField {
  String key;
  String value;
};

static void skipWhitespace(const String& input, size_t& pos) {
  while (pos < input.length() && (input[pos] == ' ' || input[pos] == '\t' || input[pos] == '\r' || input[pos] == '\n')) ++pos;
}

// This small parser accepts only flat JSON objects with boolean or integer values.
// Those are the only value types used by this firmware's POST endpoints.
static bool parseFlatJson(const String& input, JsonField* fields, size_t capacity, size_t& count) {
  count = 0;
  size_t pos = 0;
  skipWhitespace(input, pos);
  if (pos >= input.length() || input[pos++] != '{') return false;
  skipWhitespace(input, pos);
  if (pos < input.length() && input[pos] == '}') {
    ++pos;
    skipWhitespace(input, pos);
    return pos == input.length();
  }

  while (pos < input.length()) {
    skipWhitespace(input, pos);
    if (pos >= input.length() || input[pos++] != '"') return false;
    String key;
    while (pos < input.length() && input[pos] != '"') {
      // API keys are plain ASCII identifiers; escape sequences are not needed.
      if (input[pos] == '\\' || input[pos] < 0x20) return false;
      key += input[pos++];
    }
    if (pos >= input.length() || input[pos++] != '"') return false;
    skipWhitespace(input, pos);
    if (pos >= input.length() || input[pos++] != ':') return false;
    skipWhitespace(input, pos);

    String value;
    if (input.startsWith("true", pos)) {
      value = "true";
      pos += 4;
    } else if (input.startsWith("false", pos)) {
      value = "false";
      pos += 5;
    } else {
      size_t start = pos;
      if (pos < input.length() && input[pos] == '-') ++pos;
      size_t digits = pos;
      while (pos < input.length() && isDigit(input[pos])) ++pos;
      if (digits == pos) return false;
      // Reject decimals, exponents, and leading zeros; the API expects JSON integers.
      if (pos < input.length() && (input[pos] == '.' || input[pos] == 'e' || input[pos] == 'E' || input[pos] == '+')) return false;
      if (pos - digits > 1 && input[digits] == '0') return false;
      value = input.substring(start, pos);
    }

    if (count >= capacity) return false;
    for (size_t i = 0; i < count; ++i) if (fields[i].key == key) return false;
    fields[count].key = key;
    fields[count].value = value;
    ++count;

    skipWhitespace(input, pos);
    if (pos < input.length() && input[pos] == ',') {
      ++pos;
      continue;
    }
    if (pos < input.length() && input[pos] == '}') {
      ++pos;
      skipWhitespace(input, pos);
      return pos == input.length();
    }
    return false;
  }
  return false;
}

static const String* findField(const JsonField* fields, size_t count, const char* key) {
  for (size_t i = 0; i < count; ++i) if (fields[i].key == key) return &fields[i].value;
  return nullptr;
}

static bool parseMinuteValue(const String* value, uint16_t& result) {
  if (!value || value->length() == 0) return false;
  for (size_t i = 0; i < value->length(); ++i) if (!isDigit((*value)[i])) return false;
  char* end = nullptr;
  long parsed = strtol(value->c_str(), &end, 10);
  if (!end || *end != '\0' || parsed < 0 || parsed > 1439) return false;
  result = static_cast<uint16_t>(parsed);
  return true;
}

static String jsonEscape(const String& value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (static_cast<uint8_t>(c) >= 0x20) out += c;
  }
  return out;
}

static bool clockIsSynced() {
  return time(nullptr) >= VALID_CLOCK_EPOCH;
}

static uint32_t makeDayKey(const tm& local) {
  return static_cast<uint32_t>((local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday);
}

static String currentTimestamp() {
  if (!clockIsSynced()) return "Time not synced";
  time_t now = time(nullptr);
  tm local{};
  if (!localtime_r(&now, &local)) return "Time unavailable";
  char buffer[32];
  strftime(buffer, sizeof(buffer), "%b %d · %I:%M %p", &local);
  String result(buffer);
  if (result.startsWith("0")) result.remove(0, 1);
  return result;
}

static void saveHistory() {
  for (uint8_t i = 0; i < HISTORY_LIMIT; ++i) {
    if (i < historyCount) {
      String packed = historyEntries[i].label + "|" + historyEntries[i].timestamp;
      preferences.putString((String("h") + i).c_str(), packed);
    } else {
      preferences.remove((String("h") + i).c_str());
    }
  }
}

static void loadHistory() {
  historyCount = 0;
  for (uint8_t i = 0; i < HISTORY_LIMIT; ++i) {
    String packed = preferences.getString((String("h") + i).c_str(), "");
    int separator = packed.indexOf('|');
    if (separator < 0) continue;
    historyEntries[historyCount].label = packed.substring(0, separator);
    historyEntries[historyCount].timestamp = packed.substring(separator + 1);
    ++historyCount;
  }
}

static void addHistory(const char* label) {
  for (int i = HISTORY_LIMIT - 1; i > 0; --i) historyEntries[i] = historyEntries[i - 1];
  historyEntries[0].label = label;
  historyEntries[0].timestamp = currentTimestamp();
  if (historyCount < HISTORY_LIMIT) ++historyCount;
  saveHistory();
}

static void persistUsage() {
  preferences.putUInt("usageDay", usageDayKey);
  preferences.putUInt("usageSec", dailyUsageSeconds);
  lastUsageSaveMs = millis();
}

static void updateDailyUsage() {
  const uint32_t nowMs = millis();
  const uint32_t elapsedSeconds = (nowMs - lastUsageTickMs) / 1000;
  if (elapsedSeconds > 0) {
    lastUsageTickMs += elapsedSeconds * 1000;
    if (relayIsOn) dailyUsageSeconds += elapsedSeconds;
  }

  if (clockIsSynced()) {
    time_t now = time(nullptr);
    tm local{};
    if (localtime_r(&now, &local)) {
      const uint32_t today = makeDayKey(local);
      if (usageDayKey != today) {
        usageDayKey = today;
        dailyUsageSeconds = 0;
        persistUsage();
      }
    }
  }

  if (static_cast<uint32_t>(nowMs - lastUsageSaveMs) >= USAGE_SAVE_INTERVAL_MS) persistUsage();
}

static void setRelay(bool on, const char* reason) {
  updateDailyUsage();
  if (relayIsOn == on) return;
  relayIsOn = on;
  digitalWrite(RELAY_PIN, on ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
  const char* label = on ? "Light switched ON" : "Light switched OFF";
  addHistory(label);
  persistUsage();
  Serial.printf("Relay state changed: %s (%s)\n", on ? "ON" : "OFF", reason);
}

static bool scheduleShouldBeOn(uint16_t currentMinute) {
  if (onMinute == offMinute) return false;
  if (onMinute < offMinute) return currentMinute >= onMinute && currentMinute < offMinute;
  // Overnight interval, e.g. 6:40 PM to 6:40 AM.
  return currentMinute >= onMinute || currentMinute < offMinute;
}

static void evaluateSchedule() {
  if (!scheduleEnabled || !clockIsSynced()) return;
  const time_t now = time(nullptr);
  const int64_t minuteStamp = static_cast<int64_t>(now / 60);
  if (!scheduleCheckPending && minuteStamp == lastScheduleMinute) return;
  lastScheduleMinute = minuteStamp;
  scheduleCheckPending = false;

  tm local{};
  if (!localtime_r(&now, &local)) return;
  const uint16_t currentMinute = static_cast<uint16_t>(local.tm_hour * 60 + local.tm_min);
  setRelay(scheduleShouldBeOn(currentMinute), "schedule");
}

static void sendJson(int status, const String& body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(status, "application/json; charset=utf-8", body);
}

static void sendError(int status, const char* message) {
  sendJson(status, String("{\"ok\":false,\"error\":\"") + jsonEscape(message) + "\"}");
}

static String makeStateJson() {
  updateDailyUsage();
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const bool timeSynced = clockIsSynced();
  String timeText = "";
  String dateText = "";
  if (timeSynced) {
    time_t now = time(nullptr);
    tm local{};
    if (localtime_r(&now, &local)) {
      char dateBuffer[32];
      strftime(dateBuffer, sizeof(dateBuffer), "%A, %B %d", &local);
      dateText = dateBuffer;
      char timeBuffer[20];
      const int hour12 = (local.tm_hour % 12) ? (local.tm_hour % 12) : 12;
      snprintf(timeBuffer, sizeof(timeBuffer), "%d:%02d %s", hour12, local.tm_min, local.tm_hour >= 12 ? "PM" : "AM");
      timeText = timeBuffer;
    }
  }

  String json;
  json.reserve(1150);
  json += "{\"ok\":true,\"wifiConnected\":";
  json += wifiConnected ? "true" : "false";
  json += ",\"ipAddress\":\"" + jsonEscape(wifiConnected ? WiFi.localIP().toString() : String("")) + "\"";
  json += ",\"rssi\":" + String(wifiConnected ? WiFi.RSSI() : 0);
  json += ",\"relayOn\":" + String(relayIsOn ? "true" : "false");
  json += ",\"scheduleEnabled\":" + String(scheduleEnabled ? "true" : "false");
  json += ",\"onMinute\":" + String(onMinute) + ",\"offMinute\":" + String(offMinute);
  json += ",\"timeSynced\":" + String(timeSynced ? "true" : "false");
  json += ",\"timeText\":\"" + jsonEscape(timeText) + "\",\"dateText\":\"" + jsonEscape(dateText) + "\"";
  json += ",\"dailyUsageSeconds\":" + String(dailyUsageSeconds);
  json += ",\"history\":[";
  for (uint8_t i = 0; i < historyCount; ++i) {
    if (i) json += ',';
    json += "{\"label\":\"" + jsonEscape(historyEntries[i].label) + "\",\"time\":\"" + jsonEscape(historyEntries[i].timestamp) + "\"}";
  }
  json += "]}";
  return json;
}

static void handleState() {
  sendJson(200, makeStateJson());
}

static void handleControl() {
  if (!server.hasArg("plain")) return sendError(400, "Expected a JSON body.");
  if (server.arg("plain").length() > 128) return sendError(413, "JSON body is too large.");
  JsonField fields[2];
  size_t count = 0;
  const String body = server.arg("plain");
  if (!parseFlatJson(body, fields, 2, count) || count != 1 || !findField(fields, count, "on")) {
    return sendError(400, "Expected exactly one boolean field named on.");
  }
  const String* value = findField(fields, count, "on");
  if (*value != "true" && *value != "false") return sendError(400, "The on field must be true or false.");
  setRelay(*value == "true", "web control");
  sendJson(200, makeStateJson());
}

static void handleSettings() {
  if (!server.hasArg("plain")) return sendError(400, "Expected a JSON body.");
  if (server.arg("plain").length() > 128) return sendError(413, "JSON body is too large.");
  JsonField fields[4];
  size_t count = 0;
  const String body = server.arg("plain");
  if (!parseFlatJson(body, fields, 4, count) || count != 3) {
    return sendError(400, "Expected scheduleEnabled, onMinute, and offMinute.");
  }
  const String* enabledValue = findField(fields, count, "scheduleEnabled");
  const String* onValue = findField(fields, count, "onMinute");
  const String* offValue = findField(fields, count, "offMinute");
  uint16_t nextOn = 0, nextOff = 0;
  if (!enabledValue || (*enabledValue != "true" && *enabledValue != "false") ||
      !parseMinuteValue(onValue, nextOn) || !parseMinuteValue(offValue, nextOff)) {
    return sendError(400, "Use a boolean scheduleEnabled and times from 0 through 1439 minutes.");
  }
  const bool nextEnabled = *enabledValue == "true";
  if (nextEnabled && nextOn == nextOff) return sendError(400, "On and off times must differ while the schedule is enabled.");

  scheduleEnabled = nextEnabled;
  onMinute = nextOn;
  offMinute = nextOff;
  preferences.putBool("sched", scheduleEnabled);
  preferences.putUShort("onMin", onMinute);
  preferences.putUShort("offMin", offMinute);
  scheduleCheckPending = true;
  sendJson(200, makeStateJson());
}

static void connectToWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(NOVA_WIFI_SSID, NOVA_WIFI_PASSWORD);
  lastReconnectAttemptMs = millis();
  Serial.printf("Connecting to Wi-Fi network: %s\n", NOVA_WIFI_SSID);
}

static void handleWifiReconnect() {
  if (WiFi.status() == WL_CONNECTED) return;
  const uint32_t now = millis();
  if (static_cast<uint32_t>(now - lastReconnectAttemptMs) < 15000) return;
  lastReconnectAttemptMs = now;
  Serial.println("Wi-Fi disconnected; requesting reconnect.");
  WiFi.reconnect();
}

void setup() {
  Serial.begin(115200);
  delay(30);

  // Set the output latch to the inactive level before enabling the GPIO output.
  digitalWrite(RELAY_PIN, RELAY_OFF_LEVEL);
  pinMode(RELAY_PIN, OUTPUT);
  relayIsOn = false;
  Serial.printf("Relay initialized OFF on GPIO %u (active %s).\n", RELAY_PIN, RELAY_ACTIVE_LEVEL == LOW ? "LOW" : "HIGH");

  preferences.begin("novalight", false);
  scheduleEnabled = preferences.getBool("sched", false);
  onMinute = preferences.getUShort("onMin", DEFAULT_ON_MINUTE);
  offMinute = preferences.getUShort("offMin", DEFAULT_OFF_MINUTE);
  if (onMinute > 1439) onMinute = DEFAULT_ON_MINUTE;
  if (offMinute > 1439) offMinute = DEFAULT_OFF_MINUTE;
  usageDayKey = preferences.getUInt("usageDay", 0);
  dailyUsageSeconds = preferences.getUInt("usageSec", 0);
  loadHistory();
  lastUsageTickMs = millis();
  lastUsageSaveMs = lastUsageTickMs;

  connectToWifi();
  configTzTime("IST-5:30", "pool.ntp.org", "time.nist.gov");

  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-cache");
    server.send_P(200, "text/html; charset=utf-8", NOVALIGHT_HTML);
  });
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/control", HTTP_POST, handleControl);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.onNotFound([]() {
    if (server.uri().startsWith("/api/")) sendError(404, "API endpoint not found.");
    else server.send(404, "text/plain; charset=utf-8", "Not found");
  });
  server.begin();
  Serial.println("Nova Light web server started on port 80.");
  Serial.println("Open the ESP32 IP address shown below after Wi-Fi connects.");
}

void loop() {
  server.handleClient();
  handleWifiReconnect();
  updateDailyUsage();
  evaluateSchedule();

  static bool reportedConnected = false;
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !reportedConnected) {
    Serial.printf("Wi-Fi connected. IP address: %s\n", WiFi.localIP().toString().c_str());
    reportedConnected = true;
  } else if (!connected && reportedConnected) {
    Serial.println("Wi-Fi connection lost. The local server will resume when Wi-Fi reconnects.");
    reportedConnected = false;
  }
  delay(2); // Yield briefly to the ESP32 system tasks without slowing HTTP handling.
}

