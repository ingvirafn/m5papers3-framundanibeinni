
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <time.h>
#include <vector>
#include <algorithm>

// =====================================================
// WIFI CONFIGURATION
// =====================================================

#define WIFI_SSID ""
#define WIFI_PASSWORD ""

// Remove this line after entering your credentials.
#error "BUILD STOP: Update WIFI_SSID and WIFI_PASSWORD!"

// =====================================================
// CONFIGURATION
// =====================================================

const char* API_URL =
  "https://www.framundanibeinni.is/api/schedule";

const char* TIMEZONE = "GMT0";

constexpr int SCREEN_W = 540;
constexpr int SCREEN_H = 960;

constexpr int HEADER_BOTTOM = 192;
constexpr int CONTENT_TOP = 208;
constexpr int CONTENT_BOTTOM = 865;
constexpr int FOOTER_TOP = 880;

constexpr int LEFT = 20;
constexpr int RIGHT = 520;

constexpr int DAY_HEIGHT = 58;
constexpr int ROW_HEIGHT = 100;

constexpr int SLEEP_X = 255;
constexpr int SLEEP_Y = 902;
constexpr int SLEEP_W = 115;
constexpr int SLEEP_H = 46;

constexpr int REFRESH_X = 380;
constexpr int REFRESH_Y = 902;
constexpr int REFRESH_W = 140;
constexpr int REFRESH_H = 46;

constexpr unsigned long CHECK_INTERVAL_MS = 60000;
constexpr unsigned long RETRY_INTERVAL_MS = 60000;

constexpr uint64_t SLEEP_SECONDS =
  4ULL * 60ULL * 60ULL;

constexpr int HTTP_RETRIES = 3;

constexpr int MIN_VALID_YEAR = 2026;
constexpr int MAX_VALID_YEAR = 2035;

constexpr int DAYS_TO_SHOW = 3;

// =====================================================
// DATA
// =====================================================

struct Match {
  String date;
  String time;
  String name;
  String competition;
  String channel;
};

std::vector<Match> matches;

String dayKeys[DAYS_TO_SHOW];
String dayHeadings[DAYS_TO_SHOW];

String lastUpdated = "--:--";
String statusMessage;

bool scheduleReady = false;
bool clockReady = false;

int scrollOffset = 0;
int totalContentHeight = 0;

unsigned long lastCheck = 0;
unsigned long lastFetchAttempt = 0;

bool touchWasDown = false;
int touchStartY = 0;
int touchLastY = 0;
int touchLastX = 0;

volatile bool ntpSyncReceived = false;

// =====================================================
// ASCII TRANSLITERATION
// =====================================================

// Converts Icelandic UTF-8 characters to ASCII.
// Useful until we add a Unicode-capable font.

String toAscii(String s) {
  struct Replacement {
    const char* from;
    const char* to;
  };

  static const Replacement replacements[] = {
    {"Á", "A"},  {"á", "a"},
    {"É", "E"},  {"é", "e"},
    {"Í", "I"},  {"í", "i"},
    {"Ó", "O"},  {"ó", "o"},
    {"Ú", "U"},  {"ú", "u"},
    {"Ý", "Y"},  {"ý", "y"},
    {"Ð", "D"},  {"ð", "d"},
    {"Þ", "Th"}, {"þ", "th"},
    {"Æ", "Ae"}, {"æ", "ae"},
    {"Ö", "O"},  {"ö", "o"}
  };

  for (const auto& r : replacements) {
    s.replace(r.from, r.to);
  }

  // Use ASCII dash for compatibility.
  s.replace("–", "-");
  s.replace("—", "-");

  return s;
}

// =====================================================
// CHANNEL FILTER
// =====================================================

bool allowedChannel(String channel) {
  channel.toLowerCase();

  return channel == "disney" ||
         channel.startsWith("synsport");
}

String channelLabel(String channel) {
  String c = channel;
  c.toLowerCase();

  if (c == "disney")
    return "Disney+";

  if (c == "synsport")
    return "Syn Sport";

  if (c == "synsportviaplay")
    return "Syn Sport Viaplay";

  if (c == "synsportisland")
    return "Syn Sport Island";

  if (c.startsWith("synsportisland"))
    return "Syn Sport Island " + c.substring(14);

  return toAscii(channel);
}

// =====================================================
// DATE FORMATTING
// =====================================================

const char* WEEKDAYS[] = {
  "Sunnudagur",
  "Manudagur",
  "Thridjudagur",
  "Midvikudagur",
  "Fimmtudagur",
  "Fostudagur",
  "Laugardagur"
};

const char* MONTHS[] = {
  "januar", "februar", "mars", "april",
  "mai", "juni", "juli", "agust",
  "september", "oktober", "november",
  "desember"
};

bool plausibleTime(const tm& t) {
  int year = t.tm_year + 1900;

  return year >= MIN_VALID_YEAR &&
         year <= MAX_VALID_YEAR &&
         t.tm_mon >= 0 &&
         t.tm_mon <= 11 &&
         t.tm_mday >= 1 &&
         t.tm_mday <= 31;
}

String dateKey(const tm& t) {
  char buffer[16];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d", &t);
  return String(buffer);
}

String dateHeading(const tm& t) {
  return String(WEEKDAYS[t.tm_wday]) +
         " " +
         String(t.tm_mday) +
         ". " +
         String(MONTHS[t.tm_mon]);
}

bool getValidatedDays(tm days[DAYS_TO_SHOW]) {
  if (!clockReady)
    return false;

  if (!getLocalTime(&days[0], 1000))
    return false;

  if (!plausibleTime(days[0]))
    return false;

  for (int i = 1; i < DAYS_TO_SHOW; i++) {
    days[i] = days[0];
    days[i].tm_mday += i;
    days[i].tm_isdst = -1;
    mktime(&days[i]);

    if (!plausibleTime(days[i]))
      return false;
  }

  return true;
}

// =====================================================
// WIFI
// =====================================================

bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED)
    return true;

  Serial.println("Connecting WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < 15000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("WiFi connection failed");
  return false;
}

// =====================================================
// NTP
// =====================================================

void onTimeSync(struct timeval* tv) {
  ntpSyncReceived = true;
}

bool synchronizeClock(uint32_t timeoutMs = 30000) {
  if (!ensureWiFi())
    return false;

  clockReady = false;
  ntpSyncReceived = false;

  sntp_set_time_sync_notification_cb(onTimeSync);

  configTzTime(
    TIMEZONE,
    "pool.ntp.org",
    "time.google.com",
    "time.cloudflare.com"
  );

  Serial.println("Waiting for NTP...");

  unsigned long started = millis();

  while (millis() - started < timeoutMs) {
    if (ntpSyncReceived) {
      tm now;

      if (getLocalTime(&now, 1000) &&
          plausibleTime(now)) {
        clockReady = true;

        Serial.printf(
          "NTP synchronized: %04d-%02d-%02d %02d:%02d\n",
          now.tm_year + 1900,
          now.tm_mon + 1,
          now.tm_mday,
          now.tm_hour,
          now.tm_min
        );

        return true;
      }
    }

    delay(200);
  }

  Serial.println("NTP failed");
  return false;
}

// =====================================================
// HTTP AND JSON
// =====================================================

bool fetchSchedule() {
  lastFetchAttempt = millis();

  tm days[DAYS_TO_SHOW];

  if (!getValidatedDays(days)) {
    statusMessage = "Clock not synchronized";
    return false;
  }

  String newKeys[DAYS_TO_SHOW];
  String newHeadings[DAYS_TO_SHOW];

  for (int i = 0; i < DAYS_TO_SHOW; i++) {
    newKeys[i] = dateKey(days[i]);
    newHeadings[i] = dateHeading(days[i]);
  }

  for (int attempt = 1; attempt <= HTTP_RETRIES; attempt++) {
    Serial.printf(
      "HTTP attempt %d/%d\n",
      attempt,
      HTTP_RETRIES
    );

    if (!ensureWiFi()) {
      statusMessage = "WiFi unavailable";
      delay(1000);
      continue;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(30000);
    http.setReuse(false);

    if (!http.begin(client, API_URL)) {
      statusMessage = "HTTP begin failed";
      delay(1000);
      continue;
    }

    http.addHeader("Accept", "application/json");
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("Connection", "close");

    int code = http.GET();

    Serial.printf("HTTP status: %d\n", code);

    if (code != HTTP_CODE_OK) {
      statusMessage = "HTTP " + String(code);
      http.end();
      delay(1000);
      continue;
    }

    int expected = http.getSize();
    String response = http.getString();

    http.end();

    Serial.printf(
      "Expected bytes: %d, received: %u\n",
      expected,
      (unsigned)response.length()
    );

    if (response.isEmpty()) {
      statusMessage = "Empty response";
      delay(1000);
      continue;
    }

    if (expected >= 0 &&
        response.length() != (size_t)expected) {
      statusMessage = "Truncated response";
      delay(1500);
      continue;
    }

    response.trim();

    if (!response.startsWith("{") ||
        !response.endsWith("}")) {
      statusMessage = "Incomplete JSON";
      delay(1500);
      continue;
    }

    JsonDocument filter;
    JsonObject scheduleFilter =
      filter["schedule"].to<JsonObject>();

    for (int i = 0; i < DAYS_TO_SHOW; i++) {
      scheduleFilter[newKeys[i]] = true;
    }

    JsonDocument doc;

    DeserializationError error = deserializeJson(
      doc,
      response,
      DeserializationOption::Filter(filter)
    );

    Serial.printf("JSON: %s\n", error.c_str());

    if (error) {
      statusMessage =
        String("JSON: ") + error.c_str();

      delay(1500);
      continue;
    }

    JsonObject schedule =
      doc["schedule"].as<JsonObject>();

    if (schedule.isNull()) {
      statusMessage = "Missing schedule";
      delay(1500);
      continue;
    }

    std::vector<Match> newMatches;

    for (int i = 0; i < DAYS_TO_SHOW; i++) {
      JsonArray events =
        schedule[newKeys[i]].as<JsonArray>();

      if (events.isNull())
        continue;

      for (JsonObject event : events) {
        String channel = event["channel"] | "";

        if (!allowedChannel(channel))
          continue;

        Match match;

        match.date = newKeys[i];
        match.time = String(event["time"] | "");
        match.name = toAscii(
          String(event["name"] | "")
        );
        match.competition = toAscii(
          String(event["sport"] | "")
        );
        match.channel = channelLabel(channel);

        if (match.name.isEmpty() ||
            match.time.isEmpty())
          continue;

        newMatches.push_back(match);
      }
    }

    std::sort(
      newMatches.begin(),
      newMatches.end(),
      [](const Match& a, const Match& b) {
        if (a.date != b.date)
          return a.date < b.date;

        return a.time < b.time;
      }
    );

    // Commit only after success.
    matches = std::move(newMatches);

    for (int i = 0; i < DAYS_TO_SHOW; i++) {
      dayKeys[i] = newKeys[i];
      dayHeadings[i] = newHeadings[i];
    }

    char updated[8];
    strftime(
      updated,
      sizeof(updated),
      "%H:%M",
      &days[0]
    );

    lastUpdated = String(updated);

    statusMessage = "";
    scheduleReady = true;
    scrollOffset = 0;

    Serial.printf(
      "Filtered matches: %u\n",
      (unsigned)matches.size()
    );

    return true;
  }

  return false;
}

// =====================================================
// DRAWING HELPERS
// =====================================================

void drawText(
  int x,
  int y,
  const String& value,
  const lgfx::IFont* font
) {
  auto& d = M5.Display;

  d.setFont(font);
  d.setTextColor(BLACK, WHITE);
  d.setCursor(x, y);
  d.print(value);
}

void drawLine(int y) {
  M5.Display.drawFastHLine(
    LEFT, y, RIGHT - LEFT, BLACK
  );
}

String shortTime(const String& time) {
  return time.length() >= 5
    ? time.substring(0, 5)
    : time;
}

String truncateText(String value, int maxChars) {
  if ((int)value.length() <= maxChars)
    return value;

  return value.substring(0, maxChars - 3) + "...";
}

void drawButton(
  int x,
  int y,
  int w,
  int h,
  const String& label
) {
  M5.Display.drawRoundRect(
    x, y, w, h, 8, BLACK
  );

  drawText(
    x + 14,
    y + 12,
    label,
    &fonts::FreeSans12pt7b
  );
}

// =====================================================
// HEADER
// =====================================================

void drawHeader() {
  auto& d = M5.Display;

  const int cx = SCREEN_W / 2;
  const int iconY = 22;

  d.drawRoundRect(
    cx - 34, iconY,
    68, 50, 7, BLACK
  );

  d.drawRect(
    cx - 25, iconY + 9,
    50, 30, BLACK
  );

  d.drawLine(
    cx - 12, iconY + 50,
    cx - 19, iconY + 60,
    BLACK
  );

  d.drawLine(
    cx + 12, iconY + 50,
    cx + 19, iconY + 60,
    BLACK
  );

  drawText(
    65,
    113,
    "ITHROTTIR I BEINNI",
    &fonts::FreeSans24pt7b
  );

  drawText(
    162,
    161,
    "FRAMUNDAN I BEINNI",
    &fonts::FreeSans12pt7b
  );

  drawLine(HEADER_BOTTOM);
}

// =====================================================
// STARTUP
// =====================================================

void drawStartup(const String& message) {
  auto& d = M5.Display;

  d.startWrite();
  d.fillScreen(WHITE);

  drawHeader();

  drawText(
    LEFT,
    320,
    message,
    &fonts::FreeSans18pt7b
  );

  d.endWrite();
}

// =====================================================
// MATCH ROW
// =====================================================

void drawMatch(const Match& m, int y) {
  // Time and name share the same Y coordinate.
  const int titleY = y + 8;

  drawText(
    LEFT,
    titleY,
    shortTime(m.time),
    &fonts::FreeSans18pt7b
  );

  drawText(
    122,
    titleY,
    truncateText(m.name, 25),
    &fonts::FreeSans18pt7b
  );

  // Competition and channel on one line.
  String details =
    m.competition + " | " + m.channel;

  drawText(
    122,
    y + 51,
    truncateText(details, 43),
    &fonts::FreeSans12pt7b
  );
}

// =====================================================
// SCHEDULE
// =====================================================

void drawSchedule() {
  auto& d = M5.Display;

  d.startWrite();
  d.fillScreen(WHITE);
  d.setTextWrap(false);

  drawHeader();

  if (!scheduleReady) {
    drawText(
      LEFT,
      280,
      "Saeki dagskra...",
      &fonts::FreeSans18pt7b
    );

    if (!statusMessage.isEmpty()) {
      drawText(
        LEFT,
        340,
        statusMessage,
        &fonts::FreeSans12pt7b
      );
    }
  } else {
    int logicalY = 0;

    d.setClipRect(
      0,
      CONTENT_TOP,
      SCREEN_W,
      CONTENT_BOTTOM - CONTENT_TOP
    );

    for (int day = 0; day < DAYS_TO_SHOW; day++) {
      int headingY =
        CONTENT_TOP + logicalY - scrollOffset;

      if (headingY + DAY_HEIGHT >= CONTENT_TOP &&
          headingY < CONTENT_BOTTOM) {
        drawText(
          LEFT,
          headingY + 10,
          dayHeadings[day],
          &fonts::FreeSans18pt7b
        );
      }

      logicalY += DAY_HEIGHT;

      int count = 0;

      for (const Match& m : matches) {
        if (m.date != dayKeys[day])
          continue;

        int rowY =
          CONTENT_TOP + logicalY - scrollOffset;

        if (rowY + ROW_HEIGHT >= CONTENT_TOP &&
            rowY < CONTENT_BOTTOM) {
          drawMatch(m, rowY);
        }

        logicalY += ROW_HEIGHT;
        count++;
      }

      if (count == 0) {
        int emptyY =
          CONTENT_TOP + logicalY - scrollOffset;

        if (emptyY >= CONTENT_TOP &&
            emptyY < CONTENT_BOTTOM) {
          drawText(
            LEFT + 10,
            emptyY,
            "Engir leikir",
            &fonts::FreeSans12pt7b
          );
        }

        logicalY += 45;
      }

      logicalY += 12;
    }

    totalContentHeight = logicalY;
    d.clearClipRect();
  }

  drawLine(FOOTER_TOP);

  drawText(
    LEFT,
    916,
    "Updated " + lastUpdated,
    &fonts::FreeSans12pt7b
  );

  drawButton(
    SLEEP_X,
    SLEEP_Y,
    SLEEP_W,
    SLEEP_H,
    "Sleep"
  );

  drawButton(
    REFRESH_X,
    REFRESH_Y,
    REFRESH_W,
    REFRESH_H,
    "Refresh"
  );

  d.endWrite();
}

// =====================================================
// DEEP SLEEP
// =====================================================

void goToSleep() {
  Serial.println("Deep sleep for 4 hours");

  esp_sleep_enable_timer_wakeup(
    SLEEP_SECONDS * 1000000ULL
  );

  Serial.flush();
  esp_deep_sleep_start();
}

// =====================================================
// REFRESH
// =====================================================

void refreshSchedule() {
  if (fetchSchedule()) {
    drawSchedule();
  } else {
    Serial.println(statusMessage);

    if (!scheduleReady)
      drawSchedule();
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(300);

  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.setRotation(0);
  M5.Display.setTextWrap(false);

  drawStartup("Starting...");

  if (!ensureWiFi()) {
    statusMessage = "WiFi unavailable";
    drawSchedule();
    return;
  }

  if (!synchronizeClock()) {
    statusMessage = "NTP synchronization failed";
    drawSchedule();
    return;
  }

  drawStartup("Saeki dagskra...");
  refreshSchedule();

  lastCheck = millis();
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  M5.update();

  auto touch = M5.Touch.getDetail();
  bool touching = touch.isPressed();

  if (touching) {
    touchLastX = touch.x;
    touchLastY = touch.y;
  }

  if (touching && !touchWasDown) {
    touchStartY = touch.y;
  }

  if (!touching && touchWasDown) {
    int deltaY = touchLastY - touchStartY;

    bool sleepPressed =
      touchLastX >= SLEEP_X &&
      touchLastX < SLEEP_X + SLEEP_W &&
      touchLastY >= SLEEP_Y &&
      touchLastY < SLEEP_Y + SLEEP_H;

    bool refreshPressed =
      touchLastX >= REFRESH_X &&
      touchLastX < REFRESH_X + REFRESH_W &&
      touchLastY >= REFRESH_Y &&
      touchLastY < REFRESH_Y + REFRESH_H;

    if (sleepPressed && abs(deltaY) < 25) {
      goToSleep();
    }
    else if (refreshPressed && abs(deltaY) < 25) {
      if (!clockReady)
        synchronizeClock();

      refreshSchedule();
    }
    else if (abs(deltaY) > 35 && scheduleReady) {
      scrollOffset -= deltaY;

      int visibleHeight =
        CONTENT_BOTTOM - CONTENT_TOP;

      int maxScroll = std::max(
        0,
        totalContentHeight - visibleHeight
      );

      scrollOffset = std::max(
        0,
        std::min(scrollOffset, maxScroll)
      );

      drawSchedule();
    }
  }

  touchWasDown = touching;

  if (millis() - lastCheck >= CHECK_INTERVAL_MS) {
    lastCheck = millis();

    if (!clockReady) {
      if (synchronizeClock())
        refreshSchedule();
    }
    else {
      tm days[DAYS_TO_SHOW];

      if (!getValidatedDays(days)) {
        clockReady = false;
      }
      else if (!scheduleReady ||
               dateKey(days[0]) != dayKeys[0]) {
        if (millis() - lastFetchAttempt >=
            RETRY_INTERVAL_MS) {
          refreshSchedule();
        }
      }
    }
  }

  delay(30);
}
