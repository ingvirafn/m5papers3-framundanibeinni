
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <time.h>
#include <vector>
#include <algorithm>

// =====================================================
// CONFIGURATION
// =====================================================

#define WIFI_SSID "YOUR_WIFI"
#define WIFI_PASSWORD "YOUR_PASSWORD"

const char* ICAL_URLS[] = {
  "http://YOUR_FIRST_CALENDAR.ics",
  "http://YOUR_SECOND_CALENDAR.ics",
  "http://YOUR_THIRD_CALENDAR.ics"
};

constexpr size_t ICAL_COUNT =
  sizeof(ICAL_URLS) / sizeof(ICAL_URLS[0]);

// =====================================================
// TV SCHEDULE SUBSCRIPTIONS
// =====================================================
//
// An entry is included when ANY subscription matches.
//
// CHANNEL: Exact match, case-insensitive.
// SPORT:   Exact match, case-insensitive.
// NAME:    Contains text, case-insensitive.
//
// Empty arrays match nothing.
//
// Example channel values:
//   disney
//   synsport
//   synsportviaplay
//   synsportisland
//
// Add or remove entries as desired.
// =====================================================

const char* scheduleChannelSubscriptions[] = {
  // "disney",
  // "synsportisland"
};

const char* scheduleSportSubscriptions[] = {
  "Bónus deild kvenna",
  "Bónus deild karla",
  "Meistaradeild karla",
  "LaLiga",
  "Premier League"
};

const char* scheduleNameSubscriptions[] = {
  "Liverpool"
};

constexpr size_t CHANNEL_SUBSCRIPTION_COUNT =
  sizeof(scheduleChannelSubscriptions) /
  sizeof(scheduleChannelSubscriptions[0]);

constexpr size_t SPORT_SUBSCRIPTION_COUNT =
  sizeof(scheduleSportSubscriptions) /
  sizeof(scheduleSportSubscriptions[0]);

constexpr size_t NAME_SUBSCRIPTION_COUNT =
  sizeof(scheduleNameSubscriptions) /
  sizeof(scheduleNameSubscriptions[0]);

// Remove after configuring WiFi and iCal URLs.
#error "BUILD STOP: Configure WiFi and iCal URLs!"

const char* SPORTS_URL =
  "https://www.framundanibeinni.is/api/schedule";

const char* TIMEZONE = "GMT0";

// After 21:00, display tomorrow by default.
constexpr int NEXT_DAY_HOUR = 21;
constexpr int NEXT_DAY_MINUTE = 0;

// Sleep after two minutes of inactivity.
constexpr unsigned long AUTO_SLEEP_MS = 120000;

// Wake automatically after four hours.
constexpr uint64_t SLEEP_SECONDS =
  4ULL * 60ULL * 60ULL;

constexpr unsigned long CHECK_MS = 60000;
constexpr unsigned long RETRY_MS = 60000;

constexpr int HTTP_RETRIES = 3;
constexpr int MAX_CAL_EVENTS = 150;
constexpr int MAX_TV_EVENTS = 100;

// =====================================================
// SCREEN LAYOUT - PAPER S3, 540 x 960
// =====================================================

constexpr int LEFT = 18;
constexpr int RIGHT = 522;

constexpr int DATE_BOTTOM = 110;

constexpr int CAL_TOP = 118;
constexpr int CAL_BOTTOM = 565;

constexpr int TV_TOP = 574;
constexpr int TV_BOTTOM = 868;

constexpr int FOOTER_TOP = 878;

constexpr int CAL_ROW_H = 69;
constexpr int TV_ROW_H = 62;

constexpr int BUTTON_Y = 897;
constexpr int BUTTON_H = 48;

constexpr int PREV_X = 18;
constexpr int PREV_W = 145;

constexpr int REFRESH_X = 174;
constexpr int REFRESH_W = 190;

constexpr int NEXT_X = 375;
constexpr int NEXT_W = 147;

// =====================================================
// DATA
// =====================================================

struct CalendarEvent {
  time_t start = 0;
  time_t end = 0;

  bool allDay = false;

  String summary;
  String location;
};

struct TvEvent {
  String time;
  String name;
  String details;
};

std::vector<CalendarEvent> calendarEvents;
std::vector<TvEvent> tvEvents;

bool clockReady = false;
bool calendarLoaded = false;
bool tvLoaded = false;

bool manualDateSelection = false;

String calendarError;
String tvError;

time_t selectedDay = 0;
time_t todayMidnight = 0;

unsigned long lastCheck = 0;
unsigned long lastAttempt = 0;
unsigned long lastInteraction = 0;

volatile bool ntpReceived = false;

bool wasTouching = false;

int lastTouchX = 0;
int lastTouchY = 0;

int firstTouchX = 0;
int firstTouchY = 0;

// =====================================================
// TEXT HELPERS
// =====================================================

String ascii(String s) {
  struct Rep {
    const char* from;
    const char* to;
  };

  static const Rep reps[] = {
    {"Á","A"}, {"á","a"},
    {"É","E"}, {"é","e"},
    {"Í","I"}, {"í","i"},
    {"Ó","O"}, {"ó","o"},
    {"Ú","U"}, {"ú","u"},
    {"Ý","Y"}, {"ý","y"},
    {"Ð","D"}, {"ð","d"},
    {"Þ","Th"}, {"þ","th"},
    {"Æ","Ae"}, {"æ","ae"},
    {"Ö","O"}, {"ö","o"},
    {"–","-"}, {"—","-"},
    {"·","-"}
  };

  for (const auto& r : reps) {
    s.replace(r.from, r.to);
  }

  return s;
}

String normalized(String value) {
  value = ascii(value);
  value.trim();
  value.toLowerCase();
  return value;
}

bool equalsIgnoreCaseAscii(
  const String& value,
  const char* subscription
) {
  if (subscription == nullptr) {
    return false;
  }

  return normalized(value) ==
         normalized(String(subscription));
}

bool containsIgnoreCaseAscii(
  const String& value,
  const char* subscription
) {
  if (subscription == nullptr) {
    return false;
  }

  String needle = normalized(String(subscription));

  if (needle.isEmpty()) {
    return false;
  }

  String haystack = normalized(value);

  return haystack.indexOf(needle) >= 0;
}

// =====================================================
// SUBSCRIPTION MATCHING
// =====================================================

bool matchesChannelSubscription(
  const String& channel
) {
  for (
    size_t i = 0;
    i < CHANNEL_SUBSCRIPTION_COUNT;
    i++
  ) {
    if (
      equalsIgnoreCaseAscii(
        channel,
        scheduleChannelSubscriptions[i]
      )
    ) {
      return true;
    }
  }

  return false;
}

bool matchesSportSubscription(
  const String& sport
) {
  for (
    size_t i = 0;
    i < SPORT_SUBSCRIPTION_COUNT;
    i++
  ) {
    if (
      equalsIgnoreCaseAscii(
        sport,
        scheduleSportSubscriptions[i]
      )
    ) {
      return true;
    }
  }

  return false;
}

bool matchesNameSubscription(
  const String& name
) {
  for (
    size_t i = 0;
    i < NAME_SUBSCRIPTION_COUNT;
    i++
  ) {
    if (
      containsIgnoreCaseAscii(
        name,
        scheduleNameSubscriptions[i]
      )
    ) {
      return true;
    }
  }

  return false;
}

bool matchesScheduleSubscription(
  const String& channel,
  const String& sport,
  const String& name
) {
  return
    matchesChannelSubscription(channel) ||
    matchesSportSubscription(sport) ||
    matchesNameSubscription(name);
}

// =====================================================
// DATE HELPERS
// =====================================================

const char* WEEKDAYS[] = {
  "SUNNUDAGUR",
  "MANUDAGUR",
  "THRIDJUDAGUR",
  "MIDVIKUDAGUR",
  "FIMMTUDAGUR",
  "FOSTUDAGUR",
  "LAUGARDAGUR"
};

const char* MONTHS[] = {
  "jan", "feb", "mar", "apr",
  "mai", "jun", "jul", "agu",
  "sep", "okt", "nov", "des"
};

bool plausible(const tm& t) {
  int year = t.tm_year + 1900;
  return year >= 2026 && year <= 2035;
}

time_t midnight(time_t timestamp) {
  tm t;
  localtime_r(&timestamp, &t);

  t.tm_hour = 0;
  t.tm_min = 0;
  t.tm_sec = 0;
  t.tm_isdst = -1;

  return mktime(&t);
}

time_t addDays(time_t timestamp, int days) {
  tm t;
  localtime_r(&timestamp, &t);

  t.tm_mday += days;
  t.tm_isdst = -1;

  return mktime(&t);
}

String dateKey(time_t timestamp) {
  tm t;
  localtime_r(&timestamp, &t);

  char buffer[16];

  strftime(
    buffer,
    sizeof(buffer),
    "%Y-%m-%d",
    &t
  );

  return String(buffer);
}

String weekdayTitle(time_t timestamp) {
  tm t;
  localtime_r(&timestamp, &t);

  return String(WEEKDAYS[t.tm_wday]);
}

String shortDate(time_t timestamp) {
  tm t;
  localtime_r(&timestamp, &t);

  return String(t.tm_mday) +
         "." +
         String(MONTHS[t.tm_mon]);
}

String hhmm(time_t timestamp) {
  tm t;
  localtime_r(&timestamp, &t);

  char buffer[8];

  strftime(
    buffer,
    sizeof(buffer),
    "%H:%M",
    &t
  );

  return String(buffer);
}

time_t getDefaultDisplayDay() {
  time_t now = time(nullptr);

  tm local;
  localtime_r(&now, &local);

  time_t day = midnight(now);

  if (
    local.tm_hour > NEXT_DAY_HOUR ||
    (
      local.tm_hour == NEXT_DAY_HOUR &&
      local.tm_min >= NEXT_DAY_MINUTE
    )
  ) {
    day = addDays(day, 1);
  }

  return day;
}

// =====================================================
// WIFI / NTP
// =====================================================

bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long started = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - started < 15000
  ) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("WiFi unavailable");
  return false;
}

void ntpCallback(struct timeval* tv) {
  ntpReceived = true;
}

bool syncClock() {
  if (!ensureWiFi()) {
    return false;
  }

  ntpReceived = false;
  clockReady = false;

  sntp_set_time_sync_notification_cb(
    ntpCallback
  );

  configTzTime(
    TIMEZONE,
    "pool.ntp.org",
    "time.google.com",
    "time.cloudflare.com"
  );

  unsigned long started = millis();

  while (millis() - started < 30000) {
    if (ntpReceived) {
      tm t;

      if (
        getLocalTime(&t, 1000) &&
        plausible(t)
      ) {
        clockReady = true;

        Serial.println("NTP synchronized");
        return true;
      }
    }

    delay(200);
  }

  Serial.println("NTP synchronization failed");
  return false;
}

// =====================================================
// HTTP DOWNLOAD
// =====================================================

bool downloadText(
  const char* url,
  String& output,
  String& error
) {
  output = "";

  for (
    int attempt = 1;
    attempt <= HTTP_RETRIES;
    attempt++
  ) {
    if (!ensureWiFi()) {
      error = "WiFi unavailable";
      delay(1000);
      continue;
    }

    WiFiClient plainClient;
    WiFiClientSecure secureClient;

    secureClient.setInsecure();

    bool https =
      String(url).startsWith("https://");

    HTTPClient http;

    http.setTimeout(30000);
    http.setReuse(false);

    http.setFollowRedirects(
      HTTPC_STRICT_FOLLOW_REDIRECTS
    );

    http.setRedirectLimit(5);

    bool started = https
      ? http.begin(secureClient, url)
      : http.begin(plainClient, url);

    if (!started) {
      error = "HTTP begin failed";
      delay(1000);
      continue;
    }

    http.addHeader(
      "Accept-Encoding",
      "identity"
    );

    http.addHeader(
      "Connection",
      "close"
    );

    int code = http.GET();

    Serial.printf(
      "HTTP attempt %d: %d (%s)\n",
      attempt,
      code,
      http.errorToString(code).c_str()
    );

    if (code != HTTP_CODE_OK) {
      error = "HTTP " + String(code);

      http.end();
      delay(1200);
      continue;
    }

    int expected = http.getSize();
    String body = http.getString();

    http.end();

    Serial.printf(
      "Received %u bytes (expected %d)\n",
      (unsigned)body.length(),
      expected
    );

    if (body.isEmpty()) {
      error = "Empty response";
      delay(1200);
      continue;
    }

    if (
      expected >= 0 &&
      body.length() != (size_t)expected
    ) {
      error = "Truncated response";
      delay(1200);
      continue;
    }

    output = std::move(body);
    error = "";

    return true;
  }

  return false;
}

// =====================================================
// ICAL HELPERS
// =====================================================

bool parseIcalTime(
  const String& value,
  time_t& result,
  bool& allDay
) {
  String s = value;
  s.trim();

  allDay = s.length() == 8;

  if (s.length() < 8) {
    return false;
  }

  tm t = {};

  t.tm_year =
    s.substring(0, 4).toInt() - 1900;

  t.tm_mon =
    s.substring(4, 6).toInt() - 1;

  t.tm_mday =
    s.substring(6, 8).toInt();

  t.tm_isdst = -1;

  if (!allDay) {
    if (s.length() < 15) {
      return false;
    }

    t.tm_hour =
      s.substring(9, 11).toInt();

    t.tm_min =
      s.substring(11, 13).toInt();

    t.tm_sec =
      s.substring(13, 15).toInt();
  }

  result = mktime(&t);

  return result != (time_t)-1;
}

String unescapeIcal(String s) {
  s.replace("\\n", " ");
  s.replace("\\N", " ");
  s.replace("\\,", ",");
  s.replace("\\;", ";");
  s.replace("\\\\", "\\");

  return s;
}

String propertyValue(const String& line) {
  int colon = line.indexOf(':');

  if (colon < 0) {
    return "";
  }

  return line.substring(colon + 1);
}

bool propertyIs(
  const String& line,
  const char* property
) {
  int colon = line.indexOf(':');

  if (colon < 0) {
    return false;
  }

  String key = line.substring(0, colon);

  int semicolon = key.indexOf(';');

  if (semicolon >= 0) {
    key = key.substring(0, semicolon);
  }

  return key.equalsIgnoreCase(property);
}

// =====================================================
// PARSE ONE ICAL FEED
// =====================================================

bool parseCalendar(
  const String& ics,
  std::vector<CalendarEvent>& output
) {
  bool inEvent = false;

  CalendarEvent current;

  String startRaw;
  String endRaw;
  String logicalLine;

  time_t dayStart = midnight(selectedDay);
  time_t dayEnd = addDays(dayStart, 1);

  auto processLine = [&](const String& line) {
    if (line == "BEGIN:VEVENT") {
      inEvent = true;

      current = CalendarEvent();

      startRaw = "";
      endRaw = "";

      return;
    }

    if (line == "END:VEVENT") {
      if (inEvent) {
        bool startAllDay = false;
        bool endAllDay = false;

        if (
          parseIcalTime(
            startRaw,
            current.start,
            startAllDay
          )
        ) {
          current.allDay = startAllDay;

          if (
            !parseIcalTime(
              endRaw,
              current.end,
              endAllDay
            )
          ) {
            current.end = current.start;
          }

          if (current.end <= current.start) {
            current.end =
              current.start +
              (current.allDay ? 86400 : 3600);
          }

          if (
            current.start < dayEnd &&
            current.end > dayStart &&
            output.size() < MAX_CAL_EVENTS
          ) {
            output.push_back(current);
          }
        }
      }

      inEvent = false;
      return;
    }

    if (!inEvent) {
      return;
    }

    if (propertyIs(line, "DTSTART")) {
      startRaw = propertyValue(line);
    }
    else if (propertyIs(line, "DTEND")) {
      endRaw = propertyValue(line);
    }
    else if (propertyIs(line, "SUMMARY")) {
      current.summary = unescapeIcal(
        propertyValue(line)
      );
    }
    else if (propertyIs(line, "LOCATION")) {
      current.location = unescapeIcal(
        propertyValue(line)
      );
    }
  };

  int pos = 0;

  while (pos < (int)ics.length()) {
    int next = ics.indexOf('\n', pos);

    if (next < 0) {
      next = ics.length();
    }

    String line = ics.substring(pos, next);

    if (line.endsWith("\r")) {
      line.remove(line.length() - 1);
    }

    pos = next + 1;

    if (
      line.startsWith(" ") ||
      line.startsWith("\t")
    ) {
      logicalLine += line.substring(1);
    }
    else {
      if (!logicalLine.isEmpty()) {
        processLine(logicalLine);
      }

      logicalLine = line;
    }
  }

  if (!logicalLine.isEmpty()) {
    processLine(logicalLine);
  }

  return true;
}

// =====================================================
// FETCH AND MERGE ALL CALENDARS
// =====================================================

bool fetchCalendar() {
  std::vector<CalendarEvent> merged;

  int successfulFeeds = 0;
  String errors;

  for (size_t i = 0; i < ICAL_COUNT; i++) {
    Serial.printf(
      "Fetching calendar %u/%u\n",
      (unsigned)(i + 1),
      (unsigned)ICAL_COUNT
    );

    String body;
    String error;

    if (
      !downloadText(
        ICAL_URLS[i],
        body,
        error
      )
    ) {
      Serial.printf(
        "Calendar %u failed: %s\n",
        (unsigned)(i + 1),
        error.c_str()
      );

      errors +=
        "Calendar " +
        String(i + 1) +
        " failed; ";

      continue;
    }

    body.trim();

    if (
      !body.startsWith("BEGIN:VCALENDAR") ||
      body.indexOf("END:VCALENDAR") < 0
    ) {
      errors +=
        "Calendar " +
        String(i + 1) +
        " invalid; ";

      continue;
    }

    size_t before = merged.size();

    parseCalendar(body, merged);

    Serial.printf(
      "Calendar %u added %u events\n",
      (unsigned)(i + 1),
      (unsigned)(merged.size() - before)
    );

    successfulFeeds++;

    body = "";
  }

  std::sort(
    merged.begin(),
    merged.end(),
    [](const CalendarEvent& a,
       const CalendarEvent& b) {
      if (a.start != b.start) {
        return a.start < b.start;
      }

      return a.summary < b.summary;
    }
  );

  if (successfulFeeds > 0) {
    calendarEvents = std::move(merged);
    calendarLoaded = true;
    calendarError = errors;

    Serial.printf(
      "Merged calendar: %u events, %d/%u feeds\n",
      (unsigned)calendarEvents.size(),
      successfulFeeds,
      (unsigned)ICAL_COUNT
    );

    return true;
  }

  calendarLoaded = false;
  calendarError = errors;

  return false;
}

// =====================================================
// SPORTS CHANNEL LABELS
// =====================================================

String channelLabel(String channel) {
  channel.toLowerCase();

  if (channel == "disney") {
    return "Disney+";
  }

  if (channel == "synsportviaplay") {
    return "Syn Sport Viaplay";
  }

  if (channel == "synsportisland") {
    return "Syn Sport Island";
  }

  if (channel == "synsport") {
    return "Syn Sport";
  }

  return ascii(channel);
}

// =====================================================
// SPORTS FETCH WITH SUBSCRIPTION FILTERING
// =====================================================

bool fetchSports() {
  String body;
  String error;

  if (
    !downloadText(
      SPORTS_URL,
      body,
      error
    )
  ) {
    tvError = error;
    return false;
  }

  body.trim();

  if (
    !body.startsWith("{") ||
    !body.endsWith("}")
  ) {
    tvError = "Incomplete JSON";
    return false;
  }

  String key = dateKey(selectedDay);

  JsonDocument filter;
  filter["schedule"][key] = true;

  JsonDocument doc;

  DeserializationError jsonError =
    deserializeJson(
      doc,
      body,
      DeserializationOption::Filter(filter)
    );

  if (jsonError) {
    tvError =
      String("JSON: ") +
      jsonError.c_str();

    return false;
  }

  JsonObject schedule =
    doc["schedule"].as<JsonObject>();

  if (schedule.isNull()) {
    tvError = "Missing schedule";
    return false;
  }

  std::vector<TvEvent> parsed;

  JsonArray events =
    schedule[key].as<JsonArray>();

  int totalEntries = 0;
  int matchedEntries = 0;

  if (!events.isNull()) {
    for (JsonObject item : events) {
      totalEntries++;

      String channel =
        item["channel"] | "";

      String sport =
        item["sport"] | "";

      String name =
        item["name"] | "";

      // Include the event if it matches ANY
      // of the three subscription lists.
      if (
        !matchesScheduleSubscription(
          channel,
          sport,
          name
        )
      ) {
        continue;
      }

      matchedEntries++;

      TvEvent event;

      String eventTime =
        item["time"] | "";

      event.time =
        eventTime.length() >= 5
          ? eventTime.substring(0, 5)
          : eventTime;

      event.name = ascii(name);

      event.details =
        ascii(sport) +
        " | " +
        channelLabel(channel);

      if (
        !event.name.isEmpty() &&
        !event.time.isEmpty() &&
        parsed.size() < MAX_TV_EVENTS
      ) {
        parsed.push_back(event);
      }
    }
  }

  std::sort(
    parsed.begin(),
    parsed.end(),
    [](const TvEvent& a,
       const TvEvent& b) {
      return a.time < b.time;
    }
  );

  tvEvents = std::move(parsed);

  tvLoaded = true;
  tvError = "";

  Serial.printf(
    "TV schedule %s: %d entries, "
    "%d subscription matches, "
    "%u displayed\n",
    key.c_str(),
    totalEntries,
    matchedEntries,
    (unsigned)tvEvents.size()
  );

  return true;
}

// =====================================================
// DRAW HELPERS
// =====================================================

void textAt(
  int x,
  int y,
  String value,
  const lgfx::IFont* font
) {
  M5.Display.setFont(font);
  M5.Display.setTextColor(BLACK, WHITE);
  M5.Display.setCursor(x, y);
  M5.Display.print(ascii(value));
}

void lineAt(int y) {
  M5.Display.drawFastHLine(
    LEFT,
    y,
    RIGHT - LEFT,
    BLACK
  );
}

String clipText(
  String s,
  int maxChars
) {
  s = ascii(s);

  if ((int)s.length() <= maxChars) {
    return s;
  }

  return s.substring(
    0,
    maxChars - 3
  ) + "...";
}

void drawButton(
  int x,
  int y,
  int width,
  const String& label
) {
  M5.Display.drawRoundRect(
    x,
    y,
    width,
    BUTTON_H,
    7,
    BLACK
  );

  textAt(
    x + 22,
    y + 12,
    label,
    &fonts::FreeSans12pt7b
  );
}

// =====================================================
// DATE HEADER
// =====================================================

void drawDateHeader() {
  textAt(
    LEFT + 4,
    12,
    shortDate(selectedDay),
    &fonts::FreeSans12pt7b
  );

  textAt(
    LEFT,
    48,
    weekdayTitle(selectedDay),
    &fonts::FreeSans24pt7b
  );

  lineAt(DATE_BOTTOM);
}

// =====================================================
// CALENDAR SECTION
// =====================================================

void drawCalendarSection() {
  textAt(
    LEFT,
    CAL_TOP + 7,
    "DAGATAL",
    &fonts::FreeSans18pt7b
  );

  int y = CAL_TOP + 49;
  int shown = 0;

  if (!calendarLoaded) {
    textAt(
      LEFT,
      y,
      calendarError.isEmpty()
        ? "Loading calendar..."
        : calendarError,
      &fonts::FreeSans12pt7b
    );

    return;
  }

  for (const CalendarEvent& e : calendarEvents) {
    if (y + CAL_ROW_H > CAL_BOTTOM) {
      break;
    }

    String startText =
      e.allDay
        ? "ALL DAY"
        : hhmm(e.start);

    String endText =
      e.allDay
        ? ""
        : hhmm(e.end);

    textAt(
      LEFT,
      y + 2,
      startText,
      e.allDay
        ? &fonts::FreeSans12pt7b
        : &fonts::FreeSans18pt7b
    );

    if (!endText.isEmpty()) {
      textAt(
        LEFT + 4,
        y + 34,
        endText,
        &fonts::FreeSans12pt7b
      );
    }

    textAt(
      125,
      y + 2,
      clipText(e.summary, 26),
      &fonts::FreeSans18pt7b
    );

    if (!e.location.isEmpty()) {
      textAt(
        125,
        y + 34,
        clipText(e.location, 38),
        &fonts::FreeSans12pt7b
      );
    }

    y += CAL_ROW_H;
    shown++;
  }

  if (calendarEvents.empty()) {
    textAt(
      LEFT,
      y,
      "Engir vidburdir",
      &fonts::FreeSans12pt7b
    );
  }
  else if (
    calendarEvents.size() > (size_t)shown
  ) {
    textAt(
      LEFT,
      CAL_BOTTOM - 24,
      "+" +
        String(calendarEvents.size() - shown) +
        " more",
      &fonts::FreeSans12pt7b
    );
  }
}

// =====================================================
// TV SECTION
// =====================================================

void drawTvSection() {
  lineAt(TV_TOP - 5);

  textAt(
    LEFT,
    TV_TOP + 9,
    "FRAMUNDAN I BEINNI",
    &fonts::FreeSans18pt7b
  );

  int y = TV_TOP + 53;
  int shown = 0;

  if (!tvLoaded) {
    textAt(
      LEFT,
      y,
      tvError.isEmpty()
        ? "Loading TV..."
        : tvError,
      &fonts::FreeSans12pt7b
    );

    return;
  }

  for (const TvEvent& e : tvEvents) {
    if (y + TV_ROW_H > TV_BOTTOM) {
      break;
    }

    textAt(
      LEFT,
      y + 2,
      e.time,
      &fonts::FreeSans18pt7b
    );

    textAt(
      125,
      y + 2,
      clipText(e.name, 26),
      &fonts::FreeSans18pt7b
    );

    textAt(
      125,
      y + 34,
      clipText(e.details, 43),
      &fonts::FreeSans12pt7b
    );

    y += TV_ROW_H;
    shown++;
  }

  if (tvEvents.empty()) {
    textAt(
      LEFT,
      y,
      "Engir leikir",
      &fonts::FreeSans12pt7b
    );
  }
  else if (
    tvEvents.size() > (size_t)shown
  ) {
    textAt(
      LEFT,
      TV_BOTTOM - 22,
      "+" +
        String(tvEvents.size() - shown) +
        " more",
      &fonts::FreeSans12pt7b
    );
  }
}

// =====================================================
// FOOTER
// =====================================================

void drawFooter() {
  lineAt(FOOTER_TOP);

  drawButton(
    PREV_X,
    BUTTON_Y,
    PREV_W,
    "PREV"
  );

  drawButton(
    REFRESH_X,
    BUTTON_Y,
    REFRESH_W,
    "REFRESH"
  );

  drawButton(
    NEXT_X,
    BUTTON_Y,
    NEXT_W,
    "NEXT"
  );
}

// =====================================================
// SCREEN
// =====================================================

void drawScreen() {
  auto& d = M5.Display;

  d.startWrite();

  d.fillScreen(WHITE);
  d.setTextWrap(false);

  drawDateHeader();
  drawCalendarSection();
  drawTvSection();
  drawFooter();

  d.endWrite();
}

void drawStartup(String message) {
  M5.Display.fillScreen(WHITE);

  textAt(
    LEFT,
    55,
    "DAILY DASHBOARD",
    &fonts::FreeSans24pt7b
  );

  textAt(
    LEFT,
    250,
    message,
    &fonts::FreeSans18pt7b
  );
}

// =====================================================
// REFRESH
// =====================================================

void refreshAll() {
  lastAttempt = millis();

  calendarLoaded = false;
  tvLoaded = false;

  drawStartup("Fetching calendars...");
  fetchCalendar();

  drawStartup("Fetching TV schedule...");
  fetchSports();

  drawScreen();
}

// =====================================================
// NAVIGATION
// =====================================================

void changeDay(int delta) {
  manualDateSelection = true;

  selectedDay = midnight(
    addDays(selectedDay, delta)
  );

  Serial.printf(
    "Selected date: %s\n",
    dateKey(selectedDay).c_str()
  );

  refreshAll();
}

// =====================================================
// AUTOMATIC DATE SWITCH
// =====================================================

void checkDefaultDay() {
  todayMidnight = midnight(time(nullptr));

  if (manualDateSelection) {
    return;
  }

  time_t defaultDay = getDefaultDisplayDay();

  if (selectedDay != defaultDay) {
    selectedDay = defaultDay;

    Serial.printf(
      "Automatic date: %s\n",
      dateKey(selectedDay).c_str()
    );

    refreshAll();
  }
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
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(300);

  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.setRotation(0);
  M5.Display.setTextWrap(false);

  lastInteraction = millis();

  drawStartup("Connecting...");

  if (!ensureWiFi()) {
    drawStartup("WiFi unavailable");
    lastAttempt = millis();
    return;
  }

  drawStartup("Synchronizing time...");

  if (!syncClock()) {
    drawStartup("NTP synchronization failed");
    lastAttempt = millis();
    return;
  }

  todayMidnight = midnight(time(nullptr));
  selectedDay = getDefaultDisplayDay();

  manualDateSelection = false;

  Serial.printf(
    "Default date: %s\n",
    dateKey(selectedDay).c_str()
  );

  refreshAll();

  lastCheck = millis();
  lastInteraction = millis();
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  M5.update();

  auto touch = M5.Touch.getDetail();
  bool touching = touch.isPressed();

  if (touching) {
    lastTouchX = touch.x;
    lastTouchY = touch.y;

    lastInteraction = millis();
  }

  if (touching && !wasTouching) {
    firstTouchX = touch.x;
    firstTouchY = touch.y;
  }

  if (!touching && wasTouching) {
    int dx = lastTouchX - firstTouchX;
    int dy = lastTouchY - firstTouchY;

    // Tap navigation only.
    if (
      abs(dx) < 25 &&
      abs(dy) < 25 &&
      lastTouchY >= BUTTON_Y &&
      lastTouchY < BUTTON_Y + BUTTON_H
    ) {
      if (
        lastTouchX >= PREV_X &&
        lastTouchX < PREV_X + PREV_W
      ) {
        changeDay(-1);
      }
      else if (
        lastTouchX >= REFRESH_X &&
        lastTouchX < REFRESH_X + REFRESH_W
      ) {
        refreshAll();
      }
      else if (
        lastTouchX >= NEXT_X &&
        lastTouchX < NEXT_X + NEXT_W
      ) {
        changeDay(1);
      }
    }
  }

  wasTouching = touching;

  // Periodic checks.
  if (millis() - lastCheck >= CHECK_MS) {
    lastCheck = millis();

    if (!clockReady) {
      if (syncClock()) {
        todayMidnight = midnight(time(nullptr));
        selectedDay = getDefaultDisplayDay();

        manualDateSelection = false;

        refreshAll();
      }
    }
    else {
      checkDefaultDay();

      if (
        (!calendarLoaded || !tvLoaded) &&
        millis() - lastAttempt >= RETRY_MS
      ) {
        refreshAll();
      }
    }
  }

  // Automatic sleep, timer wake after four hours.
  if (
    millis() - lastInteraction >= AUTO_SLEEP_MS
  ) {
    goToSleep();
  }

  delay(30);
}
