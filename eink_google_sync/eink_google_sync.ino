#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <mbedtls/base64.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <stddef.h>
#include <time.h>
#include <vector>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoBold24pt7b.h>

#include "api_config.h"

// Optional LAN relay (the Synology NAS). Leave empty to use only the
// Cloudflare relay in GOOGLE_API_URL.
#ifndef EINK_LOCAL_RELAY_URL
#define EINK_LOCAL_RELAY_URL ""
#endif

#ifndef EINK_OTA_PASSWORD
#define EINK_OTA_PASSWORD ""
#endif

#ifndef EINK_HEALTH_WARNING_STALE_SECONDS
#define EINK_HEALTH_WARNING_STALE_SECONDS (2UL * 60UL * 60UL)
#endif

#ifndef EINK_HEALTH_CRITICAL_STALE_SECONDS
#define EINK_HEALTH_CRITICAL_STALE_SECONDS (12UL * 60UL * 60UL)
#endif

// XIAO ESP32S3 -> Waveshare e-Paper Driver HAT Rev2.3
// VCC=3V3, GND=GND, DIN=D10, CLK=D8, CS=D1,
// DC=D2, RST=D3, BUSY=D4, PWR=D5.
static constexpr uint8_t EPD_CS   = D1;
static constexpr uint8_t EPD_DC   = D2;
static constexpr uint8_t EPD_RST  = D3;
static constexpr uint8_t EPD_BUSY = D4;
static constexpr uint8_t EPD_PWR  = D5;
static constexpr uint8_t EPD_SCK  = D8;
static constexpr uint8_t EPD_MOSI = D10;

static constexpr char WIFI_AP_NAME[] = "EInk-Calendar-Setup";
static constexpr char TIMEZONE[] = "CST-8";
static constexpr char FIRMWARE_VERSION[] = "2.6.1-serial-wifi-setup";
static constexpr uint32_t WAKE_INTERVAL_SECONDS = 15UL * 60UL;
static constexpr uint32_t FULL_REFRESH_INTERVAL_SECONDS = 24UL * 60UL * 60UL;
// Keep the device visibly alive: each successful timer-driven cloud sync
// redraws the screen, including the updated sync timestamp. Full refresh is
// intentionally used because partial updates after deep sleep are not fully
// reliable on this Driver HAT.
static constexpr bool FORCE_VISIBLE_REFRESH_ON_TIMER_SYNC = true;
static constexpr uint32_t PERSISTENT_CACHE_HEARTBEAT_SECONDS = 6UL * 60UL * 60UL;
static constexpr uint32_t OTA_WINDOW_MS = 2UL * 60UL * 1000UL;
static constexpr uint32_t OTA_UPLOAD_GRACE_MS = 3UL * 60UL * 1000UL;
static constexpr uint8_t MINIMUM_REGULAR_SLEEP_SECONDS = 60;
static constexpr uint8_t WAKE_BOUNDARY_OFFSET_SECONDS = 5;
static constexpr uint8_t MAX_PARTIAL_REFRESHES = 8;
// Requests to workers.dev are dropped at random on the home network, so allow
// more tries per wake before giving up and falling back to the retry sleep.
static constexpr uint8_t MAX_SYNC_ATTEMPTS = 5;
static constexpr uint8_t MAX_VISIBLE_EVENTS = 5;
static constexpr uint8_t MAX_VISIBLE_PLANNED_TODOS = 5;
static constexpr uint8_t MAX_VISIBLE_TODOS = 5;
static constexpr uint8_t MAX_VISIBLE_NOTES = 10;
static constexpr uint32_t MIN_VALID_UNIX_EPOCH = 1700000000UL;
static constexpr uint32_t RTC_STATE_MAGIC = 0x45494E4B; // "EINK"
static constexpr uint32_t PERSISTENT_CACHE_MAGIC = 0x45414348; // "EACH"
static constexpr uint16_t PERSISTENT_CACHE_VERSION = 2;
static constexpr char PERSISTENT_NAMESPACE[] = "einkcal";
static constexpr char PERSISTENT_SLOT_A[] = "agenda0";
static constexpr char PERSISTENT_SLOT_B[] = "agenda1";
static constexpr char OTA_HOSTNAME[] = "eink-calendar";
static constexpr char OTA_USERNAME[] = "eink";
// Bump when the persistent screen layout changes so the first boot forces a
// full redraw instead of reusing an image rendered with older coordinates.
static constexpr uint16_t RTC_STATE_VERSION = 8;

// The relay uses HTTP with HMAC-authenticated requests and AES-256-GCM
// encrypted responses. DNS is preferred so Cloudflare edge changes are picked
// up automatically; the known-good IPs remain a bounded fallback for local DNS
// instability.
static constexpr char RELAY_HOST[] = "eink-calendar-relay.YOUR_SUBDOMAIN.workers.dev";
static const IPAddress RELAY_IP_PRIMARY(104, 21, 12, 115);
static const IPAddress RELAY_IP_SECONDARY(172, 67, 152, 70);
static constexpr int32_t RELAY_CONNECT_ATTEMPT_TIMEOUT_MS = 6000;

GxEPD2_BW<GxEPD2_750_GDEY075T7, GxEPD2_750_GDEY075T7::HEIGHT / 2> display(
  GxEPD2_750_GDEY075T7(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

static constexpr int16_t NOTE_X = 18;
static constexpr int16_t NOTE_Y = 86;
// The left panel is a cloud-synced memo pad. The wider shared column on the
// right remains dedicated to today's schedule, planned work and Todos.
static constexpr int16_t NOTE_W = 318;
static constexpr int16_t NOTE_H = 376;
static constexpr int16_t SIDE_X = 352;
static constexpr int16_t SIDE_W = 430;
static constexpr int16_t TODAY_SCHEDULE_Y = 86;
static constexpr int16_t TODAY_SCHEDULE_H = 119;
static constexpr int16_t PLANNED_SCHEDULE_Y = 209;
static constexpr int16_t PLANNED_SCHEDULE_H = 119;
static constexpr int16_t TODO_Y = 332;
static constexpr int16_t TODO_H = 130;
static constexpr int16_t SECTION_TITLE_H = 28;

enum class SyncFailureReason : uint8_t
{
  None = 0,
  WiFi = 1,
  Ntp = 2,
  RelayRequest = 3,
  AuthenticationOrDecrypt = 4,
  DataParse = 5,
};

enum class DeviceHealthState : uint8_t
{
  Normal = 0,
  Warning = 1,
  Critical = 2,
};

struct EventItem
{
  char time[16];
  char title[128];
};

struct TodoItem
{
  char title[128];
  bool done;
};

struct PlannedTodoItem
{
  char date[16];
  char title[128];
};

struct NoteItem
{
  char title[128];
};

struct AgendaData
{
  bool valid;
  char date[11];
  char syncedAt[6];
  uint16_t totalEvents;
  uint16_t totalPlannedTodos;
  uint16_t totalTodos;
  uint16_t totalNotes;
  uint16_t pendingTodos;
  uint8_t eventCount;
  uint8_t plannedTodoCount;
  uint8_t todoCount;
  uint8_t noteCount;
  EventItem events[MAX_VISIBLE_EVENTS];
  PlannedTodoItem plannedTodos[MAX_VISIBLE_PLANNED_TODOS];
  TodoItem todos[MAX_VISIBLE_TODOS];
  NoteItem notes[MAX_VISIBLE_NOTES];
};

struct SyncDiagnostics
{
  int16_t httpStatus;
  uint8_t attempts;
  uint32_t durationMs;
};

struct PersistentAgendaRecord
{
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t sequence;
  uint32_t savedAtEpoch;
  AgendaData agenda;
  uint32_t checksum;
};

struct RtcState
{
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t bootCount;
  int32_t renderedDate;
  int64_t lastFullRefreshEpoch;
  uint32_t lastSuccessSyncEpoch;
  uint32_t lastKnownEpoch;
  uint8_t partialRefreshCount;
  uint8_t screenValid;
  uint8_t consecutiveFailCount;
  uint8_t lastFailReason;
  uint8_t deviceHealthState;
  uint8_t hasSuccessfulSync;
  int16_t lastHttpStatus;
  uint8_t lastSyncAttemptCount;
  uint8_t reserved;
  uint32_t lastSyncDurationMs;
  uint32_t lastPersistentSaveEpoch;
  AgendaData agenda;
  uint32_t checksum;
};

RTC_DATA_ATTR static RtcState rtcState;

static AgendaData agenda = {};
static tm shownTime = {};
static int32_t lastRenderedDate = -1;
static bool displayInitialized = false;
static bool displayNeedsInitialRefresh = true;
static bool provisioningPageShown = false;
static bool firmwarePendingVerification = false;

static WebServer otaServer(80);
static bool otaUploadAuthorized = false;
static bool otaUploadStarted = false;
static bool otaUploadSucceeded = false;
static bool otaRestartRequested = false;
static char otaExpectedSha256[65] = {0};
static mbedtls_sha256_context otaSha256Context;
static String otaUploadError;

class RelayWiFiClient : public WiFiClient
{
public:
  int connect(const char* host, uint16_t port) override
  {
    return connect(host, port, 20000);
  }

  int connect(const char* host, uint16_t port, int32_t timeout) override
  {
    if (strcmp(host, RELAY_HOST) != 0)
    {
      return WiFiClient::connect(host, port, timeout);
    }

    const int32_t attemptTimeout = timeout > 0 &&
      timeout < RELAY_CONNECT_ATTEMPT_TIMEOUT_MS ?
      timeout : RELAY_CONNECT_ATTEMPT_TIMEOUT_MS;

    // On the home network the DNS answer for workers.dev is polluted, and
    // requests to either fixed Cloudflare address are dropped at random. The
    // fixed addresses go first, alternating which one leads on each request
    // so consecutive retries do not keep landing on the same path; DNS is
    // kept as the last resort in case those addresses ever stop serving.
    static uint8_t connectRound = 0;
    const bool secondaryFirst = (connectRound++ % 2) == 1;
    const IPAddress& firstIp = secondaryFirst ? RELAY_IP_SECONDARY : RELAY_IP_PRIMARY;
    const IPAddress& secondIp = secondaryFirst ? RELAY_IP_PRIMARY : RELAY_IP_SECONDARY;
    const char* firstName = secondaryFirst ? "fixed secondary" : "fixed primary";
    const char* secondName = secondaryFirst ? "fixed primary" : "fixed secondary";

    Serial.printf("Trying Cloudflare relay via %s.\n", firstName);
    if (WiFiClient::connect(firstIp, port, attemptTimeout))
    {
      Serial.printf("Cloudflare relay connected via %s.\n", firstName);
      return 1;
    }

    Serial.printf("%s failed; trying %s.\n", firstName, secondName);
    stop();
    if (WiFiClient::connect(secondIp, port, attemptTimeout))
    {
      Serial.printf("Cloudflare relay connected via %s.\n", secondName);
      return 1;
    }

    Serial.println("Both fixed addresses failed; trying DNS.");
    stop();
    IPAddress resolved;
    if (!WiFi.hostByName(host, resolved))
    {
      Serial.println("DNS lookup failed; all Cloudflare relay connection paths failed.");
      return 0;
    }
    Serial.printf("DNS resolved relay to %s.\n", resolved.toString().c_str());
    if (WiFiClient::connect(resolved, port, attemptTimeout))
    {
      Serial.println("Cloudflare relay connected via DNS.");
      return 1;
    }

    Serial.println("All Cloudflare relay connection paths failed.");
    return 0;
  }
};

static int8_t hexNibble(char value)
{
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

static bool decodeHex(const char* input, uint8_t* output, size_t outputLength)
{
  if (!input || strlen(input) != outputLength * 2) return false;
  for (size_t index = 0; index < outputLength; ++index)
  {
    const int8_t high = hexNibble(input[index * 2]);
    const int8_t low = hexNibble(input[index * 2 + 1]);
    if (high < 0 || low < 0) return false;
    output[index] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

static void encodeHex(const uint8_t* input, size_t inputLength, char* output)
{
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  for (size_t index = 0; index < inputLength; ++index)
  {
    output[index * 2] = HEX_DIGITS[input[index] >> 4];
    output[index * 2 + 1] = HEX_DIGITS[input[index] & 0x0f];
  }
  output[inputLength * 2] = '\0';
}

static bool hmacSha256(const uint8_t* key, size_t keyLength,
                       const uint8_t* input, size_t inputLength,
                       uint8_t output[32])
{
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;

  mbedtls_md_context_t context;
  mbedtls_md_init(&context);
  int status = mbedtls_md_setup(&context, info, 1);
  if (status == 0) status = mbedtls_md_hmac_starts(&context, key, keyLength);
  if (status == 0) status = mbedtls_md_hmac_update(&context, input, inputLength);
  if (status == 0) status = mbedtls_md_hmac_finish(&context, output);
  mbedtls_md_free(&context);
  return status == 0;
}

static bool deriveSubkey(const char* label, uint8_t output[32])
{
  uint8_t rootKey[32] = {0};
  if (!decodeHex(GOOGLE_API_KEY, rootKey, sizeof(rootKey))) return false;
  const bool derived = hmacSha256(rootKey, sizeof(rootKey),
                                  reinterpret_cast<const uint8_t*>(label),
                                  strlen(label), output);
  memset(rootKey, 0, sizeof(rootKey));
  return derived;
}

static bool buildRequestAuthentication(char timestamp[16],
                                       char nonceHex[33],
                                       char signatureHex[65])
{
  const time_t now = time(nullptr);
  if (now < 1700000000) return false;
  snprintf(timestamp, 16, "%lld", static_cast<long long>(now));

  uint8_t nonce[16] = {0};
  esp_fill_random(nonce, sizeof(nonce));
  encodeHex(nonce, sizeof(nonce), nonceHex);

  String message;
  message.reserve(64);
  message += "GET\n/\n";
  message += timestamp;
  message += '\n';
  message += nonceHex;

  uint8_t authKey[32] = {0};
  uint8_t signature[32] = {0};
  const bool signedRequest =
    deriveSubkey("eink-calendar-auth-v1", authKey) &&
    hmacSha256(authKey, sizeof(authKey),
               reinterpret_cast<const uint8_t*>(message.c_str()),
               message.length(), signature);
  memset(authKey, 0, sizeof(authKey));
  if (!signedRequest) return false;

  encodeHex(signature, sizeof(signature), signatureHex);
  memset(signature, 0, sizeof(signature));
  return true;
}

static bool decodeBase64(const char* input, std::vector<uint8_t>& output)
{
  if (!input || !input[0]) return false;
  size_t decodedLength = 0;
  const size_t inputLength = strlen(input);
  int status = mbedtls_base64_decode(nullptr, 0, &decodedLength,
                                     reinterpret_cast<const uint8_t*>(input),
                                     inputLength);
  if (status != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && status != 0) return false;

  output.resize(decodedLength);
  status = mbedtls_base64_decode(output.data(), output.size(), &decodedLength,
                                 reinterpret_cast<const uint8_t*>(input),
                                 inputLength);
  if (status != 0) return false;
  output.resize(decodedLength);
  return true;
}

static bool decryptRelayEnvelope(const String& body,
                                 const char* expectedTimestamp,
                                 const char* expectedNonce,
                                 String& plaintext)
{
  JsonDocument envelope;
  if (deserializeJson(envelope, body)) return false;
  if ((envelope["v"] | 0) != 1) return false;
  if (strcmp(envelope["n"] | "", expectedNonce) != 0) return false;
  if ((envelope["t"] | 0LL) != atoll(expectedTimestamp)) return false;

  std::vector<uint8_t> iv;
  std::vector<uint8_t> sealed;
  if (!decodeBase64(envelope["iv"] | "", iv) || iv.size() != 12 ||
      !decodeBase64(envelope["data"] | "", sealed) || sealed.size() <= 16)
  {
    return false;
  }

  uint8_t encryptionKey[32] = {0};
  if (!deriveSubkey("eink-calendar-encryption-v1", encryptionKey)) return false;

  String aad;
  aad.reserve(64);
  aad += "v1\n";
  aad += expectedTimestamp;
  aad += '\n';
  aad += expectedNonce;

  const size_t ciphertextLength = sealed.size() - 16;
  std::vector<uint8_t> decrypted(ciphertextLength + 1, 0);
  mbedtls_gcm_context context;
  mbedtls_gcm_init(&context);
  int status = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES,
                                  encryptionKey, sizeof(encryptionKey) * 8);
  if (status == 0)
  {
    status = mbedtls_gcm_auth_decrypt(
      &context,
      ciphertextLength,
      iv.data(), iv.size(),
      reinterpret_cast<const uint8_t*>(aad.c_str()), aad.length(),
      sealed.data() + ciphertextLength, 16,
      sealed.data(), decrypted.data());
  }
  mbedtls_gcm_free(&context);
  memset(encryptionKey, 0, sizeof(encryptionKey));
  if (status != 0) return false;

  plaintext = String(reinterpret_cast<const char*>(decrypted.data()));
  return true;
}

static void useChineseFont(uint16_t color = GxEPD_BLACK)
{
  u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312b);
  u8g2Fonts.setFontMode(1);
  u8g2Fonts.setFontDirection(0);
  u8g2Fonts.setForegroundColor(color);
  u8g2Fonts.setBackgroundColor(color == GxEPD_BLACK ? GxEPD_WHITE : GxEPD_BLACK);
}

static void useChineseListFont(uint16_t color = GxEPD_BLACK)
{
  u8g2Fonts.setFont(u8g2_font_wqy14_t_gb2312b);
  u8g2Fonts.setFontMode(1);
  u8g2Fonts.setFontDirection(0);
  u8g2Fonts.setForegroundColor(color);
  u8g2Fonts.setBackgroundColor(color == GxEPD_BLACK ? GxEPD_WHITE : GxEPD_BLACK);
}

static void drawChinese(int16_t x, int16_t baselineY, const char* text,
                        uint16_t color = GxEPD_BLACK)
{
  useChineseFont(color);
  u8g2Fonts.drawUTF8(x, baselineY, text);
}

static void drawChineseCentered(int16_t centerX, int16_t baselineY,
                                const char* text, uint16_t color = GxEPD_BLACK)
{
  useChineseFont(color);
  const int16_t width = u8g2Fonts.getUTF8Width(text);
  u8g2Fonts.drawUTF8(centerX - width / 2, baselineY, text);
}

static String fitUtf8Text(const char* text, int16_t maxWidth)
{
  useChineseFont();
  String result(text ? text : "");
  if (u8g2Fonts.getUTF8Width(result.c_str()) <= maxWidth) return result;

  while (result.length() > 0)
  {
    int cut = static_cast<int>(result.length()) - 1;
    while (cut > 0 && (static_cast<uint8_t>(result[cut]) & 0xC0) == 0x80) --cut;
    result.remove(cut);

    String candidate = result + "..";
    if (u8g2Fonts.getUTF8Width(candidate.c_str()) <= maxWidth)
    {
      return candidate;
    }
  }
  return "..";
}

static void drawChineseClipped(int16_t x, int16_t baselineY, const char* text,
                               int16_t maxWidth, uint16_t color = GxEPD_BLACK)
{
  const String fitted = fitUtf8Text(text, maxWidth);
  drawChinese(x, baselineY, fitted.c_str(), color);
}

static String fitUtf8ListText(const char* text, int16_t maxWidth)
{
  useChineseListFont();
  String result(text ? text : "");
  if (u8g2Fonts.getUTF8Width(result.c_str()) <= maxWidth) return result;

  while (result.length() > 0)
  {
    int cut = static_cast<int>(result.length()) - 1;
    while (cut > 0 && (static_cast<uint8_t>(result[cut]) & 0xC0) == 0x80) --cut;
    result.remove(cut);

    String candidate = result + "..";
    if (u8g2Fonts.getUTF8Width(candidate.c_str()) <= maxWidth)
    {
      return candidate;
    }
  }
  return "..";
}

static void drawChineseList(int16_t x, int16_t baselineY, const char* text,
                            uint16_t color = GxEPD_BLACK)
{
  useChineseListFont(color);
  u8g2Fonts.drawUTF8(x, baselineY, text);
}

static void drawChineseListClipped(int16_t x, int16_t baselineY, const char* text,
                                   int16_t maxWidth, uint16_t color = GxEPD_BLACK)
{
  const String fitted = fitUtf8ListText(text, maxWidth);
  drawChineseList(x, baselineY, fitted.c_str(), color);
}

static void drawGfxCentered(const char* text, int16_t centerX, int16_t centerY)
{
  int16_t x1, y1;
  uint16_t width, height;
  display.getTextBounds(text, 0, 0, &x1, &y1, &width, &height);
  display.setCursor(centerX - static_cast<int16_t>(width) / 2 - x1,
                    centerY - static_cast<int16_t>(height) / 2 - y1);
  display.print(text);
}

static bool isLeapYear(int year)
{
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static uint8_t daysInMonth(int year, uint8_t month)
{
  static const uint8_t DAYS[] = {31, 28, 31, 30, 31, 30,
                                 31, 31, 30, 31, 30, 31};
  if (month == 2 && isLeapYear(year)) return 29;
  return DAYS[month - 1];
}

static uint8_t mondayFirstColumn(int year, uint8_t month)
{
  tm firstDay = {};
  firstDay.tm_year = year - 1900;
  firstDay.tm_mon = month - 1;
  firstDay.tm_mday = 1;
  firstDay.tm_hour = 12;
  firstDay.tm_isdst = -1;
  mktime(&firstDay);
  return (firstDay.tm_wday + 6) % 7;
}

static uint8_t isoWeekNumber(const tm& value)
{
  char weekText[4] = {};
  if (strftime(weekText, sizeof(weekText), "%V", &value) > 0)
  {
    const int week = atoi(weekText);
    if (week >= 1 && week <= 53) return static_cast<uint8_t>(week);
  }
  return static_cast<uint8_t>((value.tm_yday + 7) / 7);
}

static int32_t dateKey(const tm& value)
{
  return (value.tm_year + 1900) * 1000L + value.tm_yday;
}

static bool isValidUnixEpoch(time_t value)
{
  return value >= static_cast<time_t>(MIN_VALID_UNIX_EPOCH);
}

static uint32_t systemEpochOrZero()
{
  const time_t now = time(nullptr);
  if (!isValidUnixEpoch(now)) return 0;
  // On ESP32 Arduino 2.x, time_t is signed 32-bit. Casting UINT32_MAX to
  // time_t produces -1, so comparing in time_t would reject every valid date.
  const uint64_t unsignedNow = static_cast<uint64_t>(now);
  if (unsignedNow > UINT32_MAX) return 0;
  return static_cast<uint32_t>(now);
}

static uint32_t healthNowEpoch()
{
  const uint32_t systemEpoch = systemEpochOrZero();
  if (systemEpoch > 0)
  {
    rtcState.lastKnownEpoch = systemEpoch;
    return systemEpoch;
  }
  return rtcState.lastKnownEpoch;
}

static void ensureInitialHealthReference(uint32_t now)
{
  if (now == 0) return;
  if (rtcState.lastKnownEpoch == 0) rtcState.lastKnownEpoch = now;

  // A fresh device has no successful Google sync yet. Use the first known
  // clock value as a reference, but keep the separate flag so it never shows
  // a stale-data warning before its first successful sync.
  if (!rtcState.hasSuccessfulSync && rtcState.lastSuccessSyncEpoch == 0)
  {
    rtcState.lastSuccessSyncEpoch = now;
  }
}

static uint32_t healthStaleSeconds(uint32_t now)
{
  if (!rtcState.hasSuccessfulSync || now == 0 ||
      now <= rtcState.lastSuccessSyncEpoch)
  {
    return 0;
  }
  return now - rtcState.lastSuccessSyncEpoch;
}

static DeviceHealthState calculateDeviceHealthState(uint32_t now)
{
  if (!rtcState.hasSuccessfulSync) return DeviceHealthState::Normal;

  const SyncFailureReason reason =
    static_cast<SyncFailureReason>(rtcState.lastFailReason);
  if (reason == SyncFailureReason::AuthenticationOrDecrypt)
  {
    return DeviceHealthState::Critical;
  }

  const uint32_t staleSeconds = healthStaleSeconds(now);
  if (staleSeconds > EINK_HEALTH_CRITICAL_STALE_SECONDS)
  {
    return DeviceHealthState::Critical;
  }
  if (staleSeconds > EINK_HEALTH_WARNING_STALE_SECONDS ||
      rtcState.consecutiveFailCount >= 3)
  {
    return DeviceHealthState::Warning;
  }
  return DeviceHealthState::Normal;
}

static const char* healthStateName(DeviceHealthState state)
{
  switch (state)
  {
    case DeviceHealthState::Warning: return "warning";
    case DeviceHealthState::Critical: return "critical";
    default: return "normal";
  }
}

static const char* healthFailReasonText(SyncFailureReason reason)
{
  switch (reason)
  {
    case SyncFailureReason::WiFi: return "Wi-Fi失败";
    case SyncFailureReason::Ntp: return "时间失败";
    case SyncFailureReason::RelayRequest: return "中继失败";
    case SyncFailureReason::AuthenticationOrDecrypt: return "认证失败";
    case SyncFailureReason::DataParse: return "数据异常";
    default: return "同步异常";
  }
}

static bool recordSyncOutcome(bool succeeded, SyncFailureReason reason,
                              uint32_t now)
{
  const DeviceHealthState previous =
    static_cast<DeviceHealthState>(rtcState.deviceHealthState);
  ensureInitialHealthReference(now);

  if (succeeded)
  {
    if (now > 0)
    {
      rtcState.lastSuccessSyncEpoch = now;
      rtcState.lastKnownEpoch = now;
    }
    rtcState.hasSuccessfulSync = 1;
    rtcState.consecutiveFailCount = 0;
    rtcState.lastFailReason = static_cast<uint8_t>(SyncFailureReason::None);
  }
  else
  {
    if (now > 0) rtcState.lastKnownEpoch = now;
    if (rtcState.consecutiveFailCount < UINT8_MAX)
    {
      ++rtcState.consecutiveFailCount;
    }
    rtcState.lastFailReason = static_cast<uint8_t>(reason);
  }

  const DeviceHealthState current = calculateDeviceHealthState(now);
  rtcState.deviceHealthState = static_cast<uint8_t>(current);
  Serial.printf("Health: %s, failures=%u, reason=%u, last success=%lu.\n",
                healthStateName(current),
                rtcState.consecutiveFailCount,
                rtcState.lastFailReason,
                static_cast<unsigned long>(rtcState.lastSuccessSyncEpoch));
  return current != previous;
}

static void recordSyncDiagnostics(const SyncDiagnostics& diagnostics)
{
  rtcState.lastHttpStatus = diagnostics.httpStatus;
  rtcState.lastSyncAttemptCount = diagnostics.attempts;
  rtcState.lastSyncDurationMs = diagnostics.durationMs;
  Serial.printf("Sync diagnostics: HTTP=%d, attempts=%u, duration=%lums.\n",
                rtcState.lastHttpStatus,
                rtcState.lastSyncAttemptCount,
                static_cast<unsigned long>(rtcState.lastSyncDurationMs));
}

static uint32_t retrySleepSeconds(uint8_t consecutiveFailureCount)
{
  if (consecutiveFailureCount <= 1) return 60;
  if (consecutiveFailureCount == 2) return 5UL * 60UL;
  return 15UL * 60UL;
}

static bool healthEpochToLocalTime(tm& output)
{
  const uint32_t epoch = healthNowEpoch();
  if (epoch < MIN_VALID_UNIX_EPOCH) return false;
  const time_t localEpoch = static_cast<time_t>(epoch);
  localtime_r(&localEpoch, &output);
  return output.tm_year + 1900 >= 2024;
}

static uint32_t calculateRtcChecksum(const RtcState& state)
{
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&state);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < offsetof(RtcState, checksum); ++index)
  {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  return hash;
}

static bool rtcStateIsValid()
{
  return rtcState.magic == RTC_STATE_MAGIC &&
         rtcState.version == RTC_STATE_VERSION &&
         rtcState.size == sizeof(RtcState) &&
         rtcState.screenValid <= 1 &&
         rtcState.partialRefreshCount <= MAX_PARTIAL_REFRESHES &&
         rtcState.lastFailReason <=
           static_cast<uint8_t>(SyncFailureReason::DataParse) &&
         rtcState.deviceHealthState <=
           static_cast<uint8_t>(DeviceHealthState::Critical) &&
         rtcState.hasSuccessfulSync <= 1 &&
         rtcState.lastSyncAttemptCount <= MAX_SYNC_ATTEMPTS &&
         rtcState.checksum == calculateRtcChecksum(rtcState);
}

static void resetRtcState()
{
  memset(&rtcState, 0, sizeof(rtcState));
  rtcState.magic = RTC_STATE_MAGIC;
  rtcState.version = RTC_STATE_VERSION;
  rtcState.size = sizeof(RtcState);
  rtcState.renderedDate = -1;
  rtcState.lastFailReason = static_cast<uint8_t>(SyncFailureReason::None);
  rtcState.deviceHealthState = static_cast<uint8_t>(DeviceHealthState::Normal);
  rtcState.lastHttpStatus = 0;
}

static void persistRtcState()
{
  rtcState.magic = RTC_STATE_MAGIC;
  rtcState.version = RTC_STATE_VERSION;
  rtcState.size = sizeof(RtcState);
  rtcState.renderedDate = lastRenderedDate;
  rtcState.agenda = agenda;
  rtcState.checksum = calculateRtcChecksum(rtcState);
}

static void localDateText(const tm& value, char* output, size_t outputSize)
{
  if (strftime(output, outputSize, "%Y-%m-%d", &value) == 0 && outputSize > 0)
  {
    output[0] = '\0';
  }
}

static bool apiConfigured()
{
  return strstr(GOOGLE_API_URL, "REPLACE_") == nullptr &&
         strstr(GOOGLE_API_KEY, "REPLACE_") == nullptr &&
         (strncmp(GOOGLE_API_URL, "http://", 7) == 0 ||
          strncmp(GOOGLE_API_URL, "https://", 8) == 0) &&
         strlen(GOOGLE_API_KEY) >= 32;
}

static void initializeDisplay()
{
  if (displayInitialized) return;

  pinMode(EPD_BUSY, INPUT);
  pinMode(EPD_PWR, OUTPUT);
  digitalWrite(EPD_PWR, HIGH);
  delay(200);

  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200, displayNeedsInitialRefresh, 2, false);
  display.setRotation(0);
  u8g2Fonts.begin(display);
  displayInitialized = true;
  Serial.printf("Display initialized (%s controller memory).\n",
                displayNeedsInitialRefresh ? "new" : "retained");
  displayNeedsInitialRefresh = false;
}

static void renderFullPage(void (*drawPage)())
{
  initializeDisplay();
  display.setFullWindow();
  display.firstPage();
  do
  {
    drawPage();
  }
  while (display.nextPage());
}

static void drawProvisioningPage()
{
  display.fillScreen(GxEPD_WHITE);
  display.fillRect(0, 0, 800, 74, GxEPD_BLACK);
  drawChineseCentered(400, 49, "首次使用：连接 Wi-Fi", GxEPD_WHITE);
  drawChinese(60, 128, "请在手机上完成以下三步：");
  drawChinese(80, 185, "1. 打开手机 Wi-Fi，连接热点");

  display.setFont(&FreeMonoBold12pt7b);
  display.setTextColor(GxEPD_BLACK);
  drawGfxCentered(WIFI_AP_NAME, 400, 228);

  drawChinese(80, 285, "2. 等待配网页面自动弹出");
  drawChinese(80, 335, "3. 选择家里的 2.4G Wi-Fi 并输入密码");
  display.drawRoundRect(130, 375, 540, 70, 8, GxEPD_BLACK);
  drawChineseCentered(400, 406, "如果页面没有弹出，请在浏览器打开");
  display.setFont(&FreeMonoBold12pt7b);
  display.setTextColor(GxEPD_BLACK);
  drawGfxCentered("192.168.4.1", 400, 431);
}

static void drawClockErrorPage()
{
  display.fillScreen(GxEPD_WHITE);
  display.fillRect(0, 0, 800, 74, GxEPD_BLACK);
  drawChineseCentered(400, 49, "网络已连接", GxEPD_WHITE);
  drawChineseCentered(400, 190, "暂时无法获取网络时间");
  drawChineseCentered(400, 240, "设备会在5分钟后自动重试，无需重新烧录");
  drawChineseCentered(400, 330, "请检查路由器是否可以访问互联网");
}

static void drawWiFiErrorPage()
{
  display.fillScreen(GxEPD_WHITE);
  display.fillRect(0, 0, 800, 74, GxEPD_BLACK);
  drawChineseCentered(400, 49, "Wi-Fi 连接失败", GxEPD_WHITE);
  drawChineseCentered(400, 190, "设备会在5分钟后自动重试");
  drawChineseCentered(400, 250, "请检查路由器和 Wi-Fi 信号");
  drawChineseCentered(400, 330, "如需重新配网，请按复位键重启");
}

static void drawHealthIndicator()
{
  const DeviceHealthState state =
    static_cast<DeviceHealthState>(rtcState.deviceHealthState);
  if (state == DeviceHealthState::Warning)
  {
    display.fillCircle(780, 35, 6, GxEPD_WHITE);
  }
  else if (state == DeviceHealthState::Critical)
  {
    display.fillTriangle(773, 41, 780, 27, 787, 41, GxEPD_WHITE);
  }
}

static void buildCriticalHealthText(char* output, size_t outputSize)
{
  const SyncFailureReason reason =
    static_cast<SyncFailureReason>(rtcState.lastFailReason);
  const uint32_t staleSeconds = healthStaleSeconds(rtcState.lastKnownEpoch);
  const uint32_t staleHours = staleSeconds == 0 ? 0 :
    (staleSeconds + 3599UL) / 3600UL;

  if (reason == SyncFailureReason::AuthenticationOrDecrypt && staleHours == 0)
  {
    if (rtcState.lastHttpStatus > 0)
    {
      snprintf(output, outputSize, "认证失败·HTTP%d", rtcState.lastHttpStatus);
    }
    else
    {
      snprintf(output, outputSize, "认证失败·检查密钥");
    }
    return;
  }
  if (staleHours > 0)
  {
    snprintf(output, outputSize, "数据滞后%lu小时·%s",
             static_cast<unsigned long>(staleHours),
             healthFailReasonText(reason));
    return;
  }
  snprintf(output, outputSize, "%s", healthFailReasonText(reason));
}

static void drawHeader()
{
  display.fillRect(0, 0, 800, 72, GxEPD_BLACK);

  char dayText[3];
  snprintf(dayText, sizeof(dayText), "%02d", shownTime.tm_mday);
  display.setFont(&FreeMonoBold24pt7b);
  display.setTextColor(GxEPD_WHITE);
  display.setCursor(22, 53);
  display.print(dayText);
  display.drawFastVLine(94, 13, 46, GxEPD_WHITE);

  char yearMonth[32];
  snprintf(yearMonth, sizeof(yearMonth), "%d年%d月",
           shownTime.tm_year + 1900, shownTime.tm_mon + 1);
  drawChinese(115, 31, yearMonth, GxEPD_WHITE);

  static const char* WEEKDAYS[] = {
    "星期日", "星期一", "星期二", "星期三",
    "星期四", "星期五", "星期六"
  };
  drawChinese(115, 56, WEEKDAYS[shownTime.tm_wday], GxEPD_WHITE);

  char syncText[24] = {};
  if (agenda.valid && agenda.syncedAt[0])
  {
    snprintf(syncText, sizeof(syncText), "同步%s", agenda.syncedAt);
  }
  else
  {
    snprintf(syncText, sizeof(syncText), "尚未同步");
  }
  useChineseFont(GxEPD_WHITE);
  const DeviceHealthState healthState =
    static_cast<DeviceHealthState>(rtcState.deviceHealthState);
  const int16_t syncRight = healthState == DeviceHealthState::Normal ? 780 : 760;
  const int16_t syncWidth = u8g2Fonts.getUTF8Width(syncText);
  u8g2Fonts.drawUTF8(syncRight - syncWidth, 43, syncText);
  drawHealthIndicator();

  if (healthState == DeviceHealthState::Critical)
  {
    char healthText[64] = {};
    buildCriticalHealthText(healthText, sizeof(healthText));
    drawChineseClipped(250, 56, healthText, 410, GxEPD_WHITE);
  }
  display.setTextColor(GxEPD_BLACK);
}

static void drawSectionTitle(int16_t x, int16_t y, int16_t width, const char* title,
                             uint16_t hiddenCount = 0)
{
  display.fillRoundRect(x, y, width, SECTION_TITLE_H, 7, GxEPD_BLACK);
  display.fillRect(x, y + 16, width, SECTION_TITLE_H - 16, GxEPD_BLACK);
  drawChinese(x + 16, y + 20, title, GxEPD_WHITE);

  if (hiddenCount > 0)
  {
    char moreText[16];
    snprintf(moreText, sizeof(moreText), "+%u", hiddenCount);
    display.setFont(&FreeMonoBold12pt7b);
    display.setTextColor(GxEPD_WHITE);
    int16_t textX, textY;
    uint16_t textW, textH;
    display.getTextBounds(moreText, 0, 0, &textX, &textY, &textW, &textH);
    display.setCursor(x + width - 16 - textW - textX, y + 20);
    display.print(moreText);
  }
}

static void drawNotepad()
{
  const uint16_t hiddenNoteCount =
    agenda.totalNotes > agenda.noteCount ? agenda.totalNotes - agenda.noteCount : 0;
  const int16_t contentX = NOTE_X + 16;
  const int16_t contentW = NOTE_W - 32;
  static constexpr int16_t FIRST_ROW_BASELINE = NOTE_Y + SECTION_TITLE_H + 21;
  static constexpr int16_t ROW_PITCH = 33;

  display.drawRoundRect(NOTE_X, NOTE_Y, NOTE_W, NOTE_H, 8, GxEPD_BLACK);
  drawSectionTitle(NOTE_X, NOTE_Y, NOTE_W, "记事本", hiddenNoteCount);

  // Ruled lines make the static e-paper area read visually as a notepad even
  // when it is empty. One Google Tasks title occupies one ruled line.
  for (uint8_t i = 0; i < MAX_VISIBLE_NOTES; ++i)
  {
    const int16_t lineY = FIRST_ROW_BASELINE + 10 + i * ROW_PITCH;
    display.drawFastHLine(contentX, lineY, contentW, GxEPD_BLACK);
  }

  if (!agenda.valid)
  {
    drawChineseCentered(NOTE_X + NOTE_W / 2, NOTE_Y + 176, "等待记事本同步");
    return;
  }

  if (agenda.noteCount == 0)
  {
    drawChineseCentered(NOTE_X + NOTE_W / 2, NOTE_Y + 174, "新建“记事本”清单");
    drawChineseCentered(NOTE_X + NOTE_W / 2, NOTE_Y + 204, "添加条目即可同步");
    return;
  }

  for (uint8_t i = 0; i < agenda.noteCount; ++i)
  {
    const int16_t rowY = FIRST_ROW_BASELINE + i * ROW_PITCH;
    display.fillCircle(NOTE_X + 28, rowY - 6, 2, GxEPD_BLACK);
    drawChineseListClipped(NOTE_X + 42, rowY, agenda.notes[i].title, NOTE_W - 58);
  }
}

static void drawSchedule()
{
  static constexpr int16_t y = TODAY_SCHEDULE_Y;
  static constexpr int16_t h = TODAY_SCHEDULE_H;
  const uint16_t hiddenEventCount =
    agenda.totalEvents > agenda.eventCount ? agenda.totalEvents - agenda.eventCount : 0;
  display.drawRoundRect(SIDE_X, y, SIDE_W, h, 8, GxEPD_BLACK);
  drawSectionTitle(SIDE_X, y, SIDE_W, "今日日程", hiddenEventCount);

  if (!agenda.valid)
  {
    drawChineseCentered(SIDE_X + SIDE_W / 2, y + 80,
                        apiConfigured() ? "日程同步失败" : "Google API 尚未配置");
    return;
  }

  if (agenda.eventCount == 0)
  {
    drawChineseCentered(SIDE_X + SIDE_W / 2, y + 80, "今天没有日程");
    return;
  }

  static constexpr int16_t FIRST_ROW_BASELINE = y + 45;
  static constexpr int16_t ROW_PITCH = 17;
  for (uint8_t i = 0; i < agenda.eventCount; ++i)
  {
    const int16_t rowY = FIRST_ROW_BASELINE + i * ROW_PITCH;
    drawChineseList(SIDE_X + 14, rowY, agenda.events[i].time);
    drawChineseListClipped(SIDE_X + 82, rowY, agenda.events[i].title, SIDE_W - 98);
  }
}

static void drawPlannedSchedule()
{
  static constexpr int16_t y = PLANNED_SCHEDULE_Y;
  static constexpr int16_t h = PLANNED_SCHEDULE_H;
  const uint16_t hiddenPlanCount = agenda.totalPlannedTodos > agenda.plannedTodoCount ?
    agenda.totalPlannedTodos - agenda.plannedTodoCount : 0;
  display.drawRoundRect(SIDE_X, y, SIDE_W, h, 8, GxEPD_BLACK);
  drawSectionTitle(SIDE_X, y, SIDE_W, "计划日程", hiddenPlanCount);

  if (!agenda.valid)
  {
    drawChineseCentered(SIDE_X + SIDE_W / 2, y + 80, "等待计划数据");
    return;
  }

  if (agenda.plannedTodoCount == 0)
  {
    drawChineseCentered(SIDE_X + SIDE_W / 2, y + 80, "没有计划日程");
    return;
  }

  static constexpr int16_t FIRST_ROW_BASELINE = y + 45;
  static constexpr int16_t ROW_PITCH = 17;
  for (uint8_t i = 0; i < agenda.plannedTodoCount; ++i)
  {
    const int16_t rowY = FIRST_ROW_BASELINE + i * ROW_PITCH;
    drawChineseList(SIDE_X + 14, rowY, agenda.plannedTodos[i].date);
    drawChineseListClipped(SIDE_X + 82, rowY, agenda.plannedTodos[i].title, SIDE_W - 98);
  }
}

static void drawCheckbox(int16_t x, int16_t y, bool done)
{
  display.drawRoundRect(x, y, 15, 15, 3, GxEPD_BLACK);
  if (done)
  {
    display.drawLine(x + 3, y + 7, x + 7, y + 11, GxEPD_BLACK);
    display.drawLine(x + 7, y + 11, x + 13, y + 3, GxEPD_BLACK);
  }
}

static void drawTodos()
{
  static constexpr int16_t y = TODO_Y;
  static constexpr int16_t h = TODO_H;
  const uint16_t hiddenTodoCount =
    agenda.totalTodos > agenda.todoCount ? agenda.totalTodos - agenda.todoCount : 0;
  display.drawRoundRect(SIDE_X, y, SIDE_W, h, 8, GxEPD_BLACK);
  drawSectionTitle(SIDE_X, y, SIDE_W, "待办事项", hiddenTodoCount);

  if (!agenda.valid)
  {
    drawChineseCentered(SIDE_X + SIDE_W / 2, y + 85, "等待 Google Tasks 数据");
    return;
  }

  if (agenda.todoCount == 0)
  {
    drawChineseCentered(SIDE_X + SIDE_W / 2, y + 85, "没有待办事项");
    return;
  }

  static constexpr int16_t FIRST_ROW_BASELINE = y + 48;
  static constexpr int16_t ROW_PITCH = 19;

  for (uint8_t i = 0; i < agenda.todoCount; ++i)
  {
    const int16_t rowY = FIRST_ROW_BASELINE + i * ROW_PITCH;
    drawCheckbox(SIDE_X + 16, rowY - 13, agenda.todos[i].done);
    drawChineseListClipped(SIDE_X + 42, rowY, agenda.todos[i].title, SIDE_W - 58);
    if (agenda.todos[i].done)
    {
      display.drawFastHLine(SIDE_X + 40, rowY - 5, SIDE_W - 72, GxEPD_BLACK);
    }
  }
}

static void drawDashboard()
{
  display.fillScreen(GxEPD_WHITE);
  drawHeader();
  drawNotepad();
  drawSchedule();
  drawPlannedSchedule();
  drawTodos();
}

static void drawSidePanel()
{
  display.fillRect(SIDE_X, NOTE_Y, SIDE_W, NOTE_H, GxEPD_WHITE);
  drawSchedule();
  drawPlannedSchedule();
  drawTodos();
}

static void renderDashboard(const tm& currentTime)
{
  shownTime = currentTime;
  Serial.printf("Full render for %04d-%02d-%02d...\n",
                shownTime.tm_year + 1900,
                shownTime.tm_mon + 1,
                shownTime.tm_mday);
  renderFullPage(drawDashboard);
  lastRenderedDate = dateKey(shownTime);
  Serial.println("Full dashboard render complete.");
}

static void renderPartialUpdates(bool updateHeader, bool updateAgenda)
{
  if (!updateHeader && !updateAgenda) return;

  initializeDisplay();
  Serial.printf("Partial refresh: header=%s, agenda=%s.\n",
                updateHeader ? "yes" : "no",
                updateAgenda ? "yes" : "no");

  if (updateHeader)
  {
    display.setPartialWindow(0, 0, 800, 72);
    display.firstPage();
    do
    {
      drawHeader();
    }
    while (display.nextPage());
  }

  if (updateAgenda)
  {
    display.setPartialWindow(NOTE_X, NOTE_Y, NOTE_W, NOTE_H);
    display.firstPage();
    do
    {
      display.fillRect(NOTE_X, NOTE_Y, NOTE_W, NOTE_H, GxEPD_WHITE);
      drawNotepad();
    }
    while (display.nextPage());

    display.setPartialWindow(SIDE_X, NOTE_Y, SIDE_W, NOTE_H);
    display.firstPage();
    do
    {
      drawSidePanel();
    }
    while (display.nextPage());
  }

  display.powerOff();
  Serial.println("Partial refresh complete.");
}

static void renderHealthTransition(const tm& currentTime, bool healthChanged)
{
  if (!healthChanged || !rtcState.screenValid) return;

  shownTime = currentTime;
  if (rtcState.partialRefreshCount >= MAX_PARTIAL_REFRESHES)
  {
    Serial.println("Full refresh due: health transition after partial refresh limit.");
    renderDashboard(currentTime);
    const uint32_t now = healthNowEpoch();
    if (now > 0) rtcState.lastFullRefreshEpoch = now;
    rtcState.partialRefreshCount = 0;
    rtcState.screenValid = 1;
    return;
  }

  renderPartialUpdates(true, false);
  ++rtcState.partialRefreshCount;
  Serial.printf("Partial refresh count: %u/%u.\n",
                rtcState.partialRefreshCount, MAX_PARTIAL_REFRESHES);
}

static bool sameAgenda(const AgendaData& left, const AgendaData& right)
{
  if (left.valid != right.valid || left.eventCount != right.eventCount ||
      left.plannedTodoCount != right.plannedTodoCount ||
      left.todoCount != right.todoCount || left.noteCount != right.noteCount ||
      left.totalEvents != right.totalEvents ||
      left.totalPlannedTodos != right.totalPlannedTodos ||
      left.totalTodos != right.totalTodos || left.totalNotes != right.totalNotes ||
      left.pendingTodos != right.pendingTodos ||
      strcmp(left.date, right.date) != 0)
  {
    return false;
  }

  for (uint8_t i = 0; i < left.eventCount; ++i)
  {
    if (strcmp(left.events[i].time, right.events[i].time) != 0 ||
        strcmp(left.events[i].title, right.events[i].title) != 0)
    {
      return false;
    }
  }
  for (uint8_t i = 0; i < left.plannedTodoCount; ++i)
  {
    if (strcmp(left.plannedTodos[i].date, right.plannedTodos[i].date) != 0 ||
        strcmp(left.plannedTodos[i].title, right.plannedTodos[i].title) != 0)
    {
      return false;
    }
  }
  for (uint8_t i = 0; i < left.todoCount; ++i)
  {
    if (left.todos[i].done != right.todos[i].done ||
        strcmp(left.todos[i].title, right.todos[i].title) != 0)
    {
      return false;
    }
  }
  for (uint8_t i = 0; i < left.noteCount; ++i)
  {
    if (strcmp(left.notes[i].title, right.notes[i].title) != 0)
    {
      return false;
    }
  }
  return true;
}

static uint32_t calculatePersistentCacheChecksum(
  const PersistentAgendaRecord& record)
{
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0;
       index < offsetof(PersistentAgendaRecord, checksum);
       ++index)
  {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  return hash;
}

static bool persistentStringIsTerminated(const char* value, size_t capacity)
{
  return memchr(value, '\0', capacity) != nullptr;
}

static bool persistentAgendaIsValid(const AgendaData& value)
{
  if (!value.valid || !persistentStringIsTerminated(value.date, sizeof(value.date)) ||
      !persistentStringIsTerminated(value.syncedAt, sizeof(value.syncedAt)) ||
       strlen(value.date) != 10 || value.eventCount > MAX_VISIBLE_EVENTS ||
       value.plannedTodoCount > MAX_VISIBLE_PLANNED_TODOS ||
       value.todoCount > MAX_VISIBLE_TODOS || value.noteCount > MAX_VISIBLE_NOTES ||
       value.totalEvents < value.eventCount ||
       value.totalPlannedTodos < value.plannedTodoCount ||
       value.totalTodos < value.todoCount || value.totalNotes < value.noteCount)
  {
    return false;
  }

  for (uint8_t index = 0; index < value.eventCount; ++index)
  {
    if (!persistentStringIsTerminated(value.events[index].time,
                                      sizeof(value.events[index].time)) ||
        !persistentStringIsTerminated(value.events[index].title,
                                      sizeof(value.events[index].title)))
    {
      return false;
    }
  }
  for (uint8_t index = 0; index < value.plannedTodoCount; ++index)
  {
    if (!persistentStringIsTerminated(value.plannedTodos[index].date,
                                      sizeof(value.plannedTodos[index].date)) ||
        !persistentStringIsTerminated(value.plannedTodos[index].title,
                                      sizeof(value.plannedTodos[index].title)))
    {
      return false;
    }
  }
  for (uint8_t index = 0; index < value.todoCount; ++index)
  {
    if (!persistentStringIsTerminated(value.todos[index].title,
                                      sizeof(value.todos[index].title)))
    {
      return false;
    }
  }
  for (uint8_t index = 0; index < value.noteCount; ++index)
  {
    if (!persistentStringIsTerminated(value.notes[index].title,
                                      sizeof(value.notes[index].title)))
    {
      return false;
    }
  }
  return true;
}

static bool persistentRecordIsValid(const PersistentAgendaRecord& record)
{
  return record.magic == PERSISTENT_CACHE_MAGIC &&
         record.version == PERSISTENT_CACHE_VERSION &&
         record.size == sizeof(PersistentAgendaRecord) &&
         record.sequence != 0 &&
         record.savedAtEpoch >= MIN_VALID_UNIX_EPOCH &&
         persistentAgendaIsValid(record.agenda) &&
         record.checksum == calculatePersistentCacheChecksum(record);
}

static bool readPersistentSlot(Preferences& preferences, const char* key,
                               PersistentAgendaRecord& output)
{
  if (preferences.getBytesLength(key) != sizeof(PersistentAgendaRecord))
  {
    return false;
  }
  // Keep the multi-kilobyte cache record out of loopTask's small stack.
  static PersistentAgendaRecord candidate;
  memset(&candidate, 0, sizeof(candidate));
  if (preferences.getBytes(key, &candidate, sizeof(candidate)) != sizeof(candidate) ||
      !persistentRecordIsValid(candidate))
  {
    return false;
  }
  output = candidate;
  return true;
}

static bool sequenceIsNewer(uint32_t candidate, uint32_t reference)
{
  return static_cast<int32_t>(candidate - reference) > 0;
}

static bool readBestPersistentRecord(Preferences& preferences,
                                     PersistentAgendaRecord& output)
{
  // These records are static because this function is called from setup().
  // Two records plus the caller's buffers otherwise overflow loopTask.
  static PersistentAgendaRecord slotA;
  static PersistentAgendaRecord slotB;
  memset(&slotA, 0, sizeof(slotA));
  memset(&slotB, 0, sizeof(slotB));
  const bool slotAValid = readPersistentSlot(preferences, PERSISTENT_SLOT_A, slotA);
  const bool slotBValid = readPersistentSlot(preferences, PERSISTENT_SLOT_B, slotB);
  if (!slotAValid && !slotBValid) return false;

  if (slotAValid && (!slotBValid || sequenceIsNewer(slotA.sequence, slotB.sequence)))
  {
    output = slotA;
  }
  else
  {
    output = slotB;
  }
  return true;
}

static bool loadPersistentAgendaCache(AgendaData& output, uint32_t& savedAtEpoch)
{
  Preferences preferences;
  if (!preferences.begin(PERSISTENT_NAMESPACE, true))
  {
    Serial.println("Persistent cache unavailable.");
    return false;
  }

  static PersistentAgendaRecord record;
  memset(&record, 0, sizeof(record));
  const bool loaded = readBestPersistentRecord(preferences, record);
  preferences.end();
  if (!loaded)
  {
    Serial.println("No valid persistent agenda cache found.");
    return false;
  }

  output = record.agenda;
  savedAtEpoch = record.savedAtEpoch;
  Serial.printf("Persistent agenda restored: sequence=%lu, saved=%lu.\n",
                static_cast<unsigned long>(record.sequence),
                static_cast<unsigned long>(record.savedAtEpoch));
  return true;
}

static bool savePersistentAgendaCache(const AgendaData& value,
                                      uint32_t savedAtEpoch)
{
  if (!persistentAgendaIsValid(value) || savedAtEpoch < MIN_VALID_UNIX_EPOCH)
  {
    Serial.println("Persistent agenda write skipped: invalid data or clock.");
    return false;
  }

  Preferences preferences;
  if (!preferences.begin(PERSISTENT_NAMESPACE, false))
  {
    Serial.println("Persistent agenda write failed: NVS unavailable.");
    return false;
  }

  static PersistentAgendaRecord current;
  memset(&current, 0, sizeof(current));
  const bool currentValid = readBestPersistentRecord(preferences, current);
  if (currentValid && sameAgenda(current.agenda, value) &&
      savedAtEpoch >= current.savedAtEpoch &&
      savedAtEpoch - current.savedAtEpoch < PERSISTENT_CACHE_HEARTBEAT_SECONDS)
  {
    rtcState.lastPersistentSaveEpoch = current.savedAtEpoch;
    preferences.end();
    Serial.println("Persistent agenda unchanged; flash write skipped.");
    return true;
  }

  static PersistentAgendaRecord next;
  memset(&next, 0, sizeof(next));
  next.magic = PERSISTENT_CACHE_MAGIC;
  next.version = PERSISTENT_CACHE_VERSION;
  next.size = sizeof(PersistentAgendaRecord);
  next.sequence = currentValid ? current.sequence + 1 : 1;
  if (next.sequence == 0) next.sequence = 1;
  next.savedAtEpoch = savedAtEpoch;
  next.agenda = value;
  next.checksum = calculatePersistentCacheChecksum(next);

  const char* targetKey = (next.sequence & 1U) ?
    PERSISTENT_SLOT_A : PERSISTENT_SLOT_B;
  const size_t written = preferences.putBytes(targetKey, &next, sizeof(next));
  static PersistentAgendaRecord verified;
  memset(&verified, 0, sizeof(verified));
  const bool writeVerified = written == sizeof(next) &&
    readPersistentSlot(preferences, targetKey, verified) &&
    verified.sequence == next.sequence;
  preferences.end();

  if (!writeVerified)
  {
    Serial.println("Persistent agenda write verification failed.");
    return false;
  }

  rtcState.lastPersistentSaveEpoch = savedAtEpoch;
  Serial.printf("Persistent agenda saved: slot=%s, sequence=%lu.\n",
                targetKey, static_cast<unsigned long>(next.sequence));
  return true;
}

static bool agendaMatchesLocalDate(const AgendaData& value,
                                   const tm& localTime)
{
  if (!value.valid) return false;
  char expectedDate[11] = {};
  localDateText(localTime, expectedDate, sizeof(expectedDate));
  return strcmp(value.date, expectedDate) == 0;
}

static bool localRelayConfigured()
{
  return strncmp(EINK_LOCAL_RELAY_URL, "http://", 7) == 0;
}

static bool fetchGoogleAgendaOnce(const tm& localTime, AgendaData& result,
                                  SyncFailureReason& failureReason,
                                  int16_t& httpStatus,
                                  const char* relayUrl)
{
  const bool localRelay = relayUrl != GOOGLE_API_URL;
  memset(&result, 0, sizeof(result));
  failureReason = SyncFailureReason::RelayRequest;
  httpStatus = 0;
  if (!apiConfigured())
  {
    Serial.println("Google API is not configured yet.");
    failureReason = SyncFailureReason::AuthenticationOrDecrypt;
    return false;
  }
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("Google API skipped: Wi-Fi is offline.");
    failureReason = SyncFailureReason::WiFi;
    return false;
  }

  char requestTimestamp[16] = {0};
  char requestNonce[33] = {0};
  char requestSignature[65] = {0};
  if (!buildRequestAuthentication(requestTimestamp,
                                  requestNonce,
                                  requestSignature))
  {
    Serial.println("Unable to authenticate relay request.");
    failureReason = SyncFailureReason::AuthenticationOrDecrypt;
    return false;
  }

  RelayWiFiClient relayClient;

  HTTPClient request;
  request.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (localRelay)
  {
    // The NAS relay answers from its LAN cache in well under a second, or
    // after at most ~8 s when it has to fetch live. Fail fast so the same
    // attempt can still fall back to the Cloudflare relay.
    request.setConnectTimeout(3000);
    request.setTimeout(10000);
  }
  else
  {
    // The relay normally answers from its KV snapshot within 2 s, and its
    // live Google fallback is capped at ~14 s. A connection that stays silent
    // for 20 s is dead, so give up and let the retry try another path.
    request.setTimeout(20000);
  }
  request.setUserAgent("EInkCalendar/2.0");

  const String url(relayUrl);
  if (!request.begin(relayClient, url))
  {
    Serial.println("Unable to start relay request.");
    failureReason = SyncFailureReason::RelayRequest;
    return false;
  }
  request.addHeader("x-device-time", requestTimestamp);
  request.addHeader("x-device-nonce", requestNonce);
  request.addHeader("x-device-signature", requestSignature);
  static const char* RELAY_RESPONSE_HEADERS[] = {"x-eink-source"};
  request.collectHeaders(RELAY_RESPONSE_HEADERS, 1);

  Serial.println("Fetching encrypted Google Calendar/Tasks JSON...");
  const int statusCode = request.GET();
  httpStatus = static_cast<int16_t>(statusCode);
  if (statusCode != HTTP_CODE_OK)
  {
    Serial.printf("Google API HTTP status: %d (%s)\n",
                  statusCode,
                  HTTPClient::errorToString(statusCode).c_str());
    request.end();
    failureReason = (statusCode == HTTP_CODE_UNAUTHORIZED ||
                     statusCode == HTTP_CODE_FORBIDDEN) ?
      SyncFailureReason::AuthenticationOrDecrypt :
      SyncFailureReason::RelayRequest;
    return false;
  }

  const String body = request.getString();
  const String relaySource = request.header("x-eink-source");
  Serial.printf("Relay source: %s.\n",
                relaySource.length() > 0 ? relaySource.c_str() : "legacy");
  request.end();

  String decryptedBody;
  if (!decryptRelayEnvelope(body, requestTimestamp, requestNonce, decryptedBody))
  {
    // A key or clock problem is rejected by the relay with HTTP 401, so an
    // HTTP 200 that fails to decrypt means a truncated or corrupted transfer.
    Serial.printf("Encrypted relay response could not be decrypted (%u bytes); treating as transient.\n",
                  static_cast<unsigned>(body.length()));
    failureReason = SyncFailureReason::RelayRequest;
    return false;
  }

  JsonDocument document;
  const DeserializationError jsonError = deserializeJson(document, decryptedBody);
  if (jsonError)
  {
    Serial.print("Google API JSON error: ");
    Serial.println(jsonError.c_str());
    failureReason = SyncFailureReason::DataParse;
    return false;
  }
  if (!(document["ok"] | false))
  {
    Serial.print("Google API returned error: ");
    Serial.println(document["error"] | "unknown");
    failureReason = SyncFailureReason::DataParse;
    return false;
  }

  char expectedDate[11];
  localDateText(localTime, expectedDate, sizeof(expectedDate));
  const char* responseDate = document["date"] | "";
  if (strcmp(responseDate, expectedDate) != 0)
  {
    Serial.printf("Google API date mismatch: expected %s, received %s\n",
                  expectedDate, responseDate);
    failureReason = SyncFailureReason::DataParse;
    return false;
  }

  result.valid = true;
  strlcpy(result.date, responseDate, sizeof(result.date));
  result.totalEvents = document["counts"]["events"] | 0;
  result.totalPlannedTodos = document["counts"]["planned_todos"] | 0;
  result.totalTodos = document["counts"]["todos"] | 0;
  result.totalNotes = document["counts"]["notes"] | 0;
  result.pendingTodos = document["counts"]["pending_todos"] | 0;
  snprintf(result.syncedAt, sizeof(result.syncedAt), "%02d:%02d",
           localTime.tm_hour, localTime.tm_min);

  for (JsonObjectConst item : document["events"].as<JsonArrayConst>())
  {
    if (result.eventCount >= MAX_VISIBLE_EVENTS) break;
    EventItem& event = result.events[result.eventCount++];
    strlcpy(event.time, item["time"] | "", sizeof(event.time));
    strlcpy(event.title, item["title"] | "无标题日程", sizeof(event.title));
  }

  for (JsonObjectConst item : document["planned_todos"].as<JsonArrayConst>())
  {
    if (result.plannedTodoCount >= MAX_VISIBLE_PLANNED_TODOS) break;
    PlannedTodoItem& planned = result.plannedTodos[result.plannedTodoCount++];
    strlcpy(planned.date, item["date"] | "计划", sizeof(planned.date));
    strlcpy(planned.title, item["title"] | "无标题任务", sizeof(planned.title));
  }

  for (JsonObjectConst item : document["todos"].as<JsonArrayConst>())
  {
    if (result.todoCount >= MAX_VISIBLE_TODOS) break;
    TodoItem& todo = result.todos[result.todoCount++];
    strlcpy(todo.title, item["title"] | "无标题任务", sizeof(todo.title));
    todo.done = item["done"] | false;
  }

  for (JsonObjectConst item : document["notes"].as<JsonArrayConst>())
  {
    if (result.noteCount >= MAX_VISIBLE_NOTES) break;
    NoteItem& note = result.notes[result.noteCount++];
    strlcpy(note.title, item["title"] | "无标题记事", sizeof(note.title));
  }

  Serial.printf("Google sync OK: %u/%u events, %u/%u planned, %u/%u todos, %u/%u notes.\n",
                result.eventCount, result.totalEvents,
                result.plannedTodoCount, result.totalPlannedTodos,
                result.todoCount, result.totalTodos,
                result.noteCount, result.totalNotes);
  failureReason = SyncFailureReason::None;
  return true;
}

static bool readNetworkTime(tm& currentTime, uint32_t timeoutMs)
{
  if (!getLocalTime(&currentTime, timeoutMs)) return false;
  return currentTime.tm_year + 1900 >= 2024;
}

static void configureLocalTimezone()
{
  setenv("TZ", TIMEZONE, 1);
  tzset();
}

static bool synchronizeClock(tm& currentTime)
{
  configureLocalTimezone();
  configTzTime(TIMEZONE, "ntp.aliyun.com", "ntp.tencent.com", "pool.ntp.org");
  Serial.println("Synchronizing network time...");
  for (uint8_t attempt = 1; attempt <= 5; ++attempt)
  {
    if (readNetworkTime(currentTime, 5000))
    {
      Serial.printf("Time synchronized: %04d-%02d-%02d %02d:%02d:%02d\n",
                    currentTime.tm_year + 1900, currentTime.tm_mon + 1,
                    currentTime.tm_mday, currentTime.tm_hour,
                    currentTime.tm_min, currentTime.tm_sec);
      return true;
    }
    Serial.printf("NTP attempt %u/5 failed.\n", attempt);
  }
  return false;
}

static void configModeCallback(WiFiManager* manager)
{
  Serial.println("Wi-Fi setup portal started.");
  Serial.print("Setup AP: ");
  Serial.println(manager->getConfigPortalSSID());
  if (!provisioningPageShown)
  {
    renderFullPage(drawProvisioningPage);
    provisioningPageShown = true;
  }
}

static bool waitForWiFi(uint32_t timeoutMs)
{
  const unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < timeoutMs)
  {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Saved Wi-Fi that still works never opens the setup portal, so switching the
// device to another network needs an explicit request. A host on the USB
// serial port can send "portal" within the window after a non-timer boot.
static bool serialPortalRequested(uint32_t windowMs)
{
  Serial.printf("Send \"portal\" over USB serial within %lu s to reconfigure Wi-Fi.\n",
                static_cast<unsigned long>(windowMs / 1000UL));
  String line;
  const unsigned long startedAt = millis();
  while (millis() - startedAt < windowMs)
  {
    while (Serial.available() > 0)
    {
      const char received = static_cast<char>(Serial.read());
      if (received == '\n' || received == '\r')
      {
        line.trim();
        if (line.equalsIgnoreCase("portal")) return true;
        line = "";
      }
      else if (line.length() < 16)
      {
        line += received;
      }
    }
    delay(20);
  }
  return false;
}

static bool connectWiFi(bool allowProvisioningPortal, bool forcePortal = false)
{
  WiFi.mode(WIFI_STA);
  // Modem power save can drop incoming TCP packets right after association;
  // the radio is only on for a few seconds per wake, so keep it fully awake.
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  if (forcePortal)
  {
    WiFiManager manager;
    manager.setHostname("eink-calendar");
    manager.setConnectTimeout(20);
    manager.setConfigPortalTimeout(300);
    manager.setAPCallback(configModeCallback);
    Serial.println("Wi-Fi setup portal requested over USB serial.");
    const bool connected = manager.startConfigPortal(WIFI_AP_NAME);
    if (connected)
    {
      Serial.print("Wi-Fi connected. IP: ");
      Serial.println(WiFi.localIP());
    }
    return connected;
  }

  if (!allowProvisioningPortal)
  {
    Serial.println("Connecting to saved Wi-Fi (20 second timeout)...");
    WiFi.begin();
    const bool connected = waitForWiFi(20000);
    if (connected)
    {
      Serial.print("Wi-Fi connected. IP: ");
      Serial.println(WiFi.localIP());
    }
    return connected;
  }

  WiFiManager manager;
  manager.setHostname("eink-calendar");
  manager.setConnectTimeout(20);
  manager.setConnectRetries(3);
  manager.setConfigPortalTimeout(180);
  manager.setAPCallback(configModeCallback);

  Serial.println("Connecting to saved Wi-Fi...");
  const bool connected = manager.autoConnect(WIFI_AP_NAME);
  if (connected)
  {
    Serial.print("Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
  }
  return connected;
}

static bool fetchGoogleAgendaWithRetry(tm& localTime, AgendaData& result,
                                       SyncFailureReason& failureReason,
                                       SyncDiagnostics& diagnostics)
{
  diagnostics = SyncDiagnostics();
  const unsigned long startedAt = millis();
  bool clockRefreshedForAuthentication = false;

  for (uint8_t attempt = 1; attempt <= MAX_SYNC_ATTEMPTS; ++attempt)
  {
    // Use the current wall clock for every attempt so a sync that crosses
    // midnight compares against the right date and records the real time.
    tm attemptTime = {};
    if (getLocalTime(&attemptTime, 0) && attemptTime.tm_year + 1900 >= 2024)
    {
      localTime = attemptTime;
    }
    diagnostics.attempts = attempt;
    int16_t httpStatus = 0;
    Serial.printf("Google sync attempt %u/%u.\n", attempt, MAX_SYNC_ATTEMPTS);
    // The NAS relay on the home LAN avoids the unreliable path to
    // workers.dev; the Cloudflare relay remains the fallback when the NAS is
    // off or unreachable.
    bool fetched = false;
    if (localRelayConfigured())
    {
      Serial.println("Trying local NAS relay.");
      fetched = fetchGoogleAgendaOnce(localTime, result, failureReason,
                                      httpStatus, EINK_LOCAL_RELAY_URL);
      if (!fetched) Serial.println("Local NAS relay failed; trying Cloudflare relay.");
    }
    if (!fetched)
    {
      fetched = fetchGoogleAgendaOnce(localTime, result, failureReason,
                                      httpStatus, GOOGLE_API_URL);
    }
    if (fetched)
    {
      diagnostics.httpStatus = httpStatus;
      diagnostics.durationMs = millis() - startedAt;
      return true;
    }
    diagnostics.httpStatus = httpStatus;

    if (!apiConfigured()) break;

    if (failureReason == SyncFailureReason::AuthenticationOrDecrypt)
    {
      if (clockRefreshedForAuthentication)
      {
        Serial.println("Authentication retry still failed; stopping this wake cycle.");
        break;
      }

      Serial.println("Authentication failed; refreshing NTP before one signed retry.");
      tm refreshedTime = {};
      if (!synchronizeClock(refreshedTime))
      {
        failureReason = SyncFailureReason::Ntp;
        break;
      }
      localTime = refreshedTime;
      clockRefreshedForAuthentication = true;
      continue;
    }

    if (attempt >= MAX_SYNC_ATTEMPTS) break;

    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("Wi-Fi dropped during sync; reconnecting before retry.");
      WiFi.reconnect();
      if (!waitForWiFi(10000))
      {
        failureReason = SyncFailureReason::WiFi;
        break;
      }
    }

    // The relay serves from its KV snapshot, so upstream recovery time no
    // longer matters. Dropped requests on the home network are random per
    // request rather than a sustained outage, so a short pause is enough.
    const uint32_t retryDelayMs = 3000UL;
    Serial.printf("Transient sync failure; retrying in %lums.\n",
                  static_cast<unsigned long>(retryDelayMs));
    delay(retryDelayMs);
  }

  diagnostics.durationMs = millis() - startedAt;
  return false;
}

static const char OTA_PAGE[] PROGMEM = R"OTAHTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>电子墨水日历固件更新</title>
  <style>
    body{font-family:system-ui,sans-serif;max-width:620px;margin:40px auto;padding:0 18px;color:#171717}
    .card{border:1px solid #bbb;border-radius:14px;padding:22px}
    label{display:block;margin:18px 0 7px;font-weight:600}
    input{width:100%;box-sizing:border-box;padding:10px}
    button{margin-top:22px;padding:11px 20px;border:0;border-radius:9px;background:#111;color:#fff;font-size:16px}
    button:disabled{opacity:.5}.hint{color:#666;line-height:1.6}.status{margin-top:18px;white-space:pre-wrap}
  </style>
</head>
<body>
  <h1>固件更新</h1>
  <div class="card">
    <p class="hint">请选择同一次构建生成的 .bin 固件和 .sha256 校验文件。校验通过后设备才会写入新版本，并在重启后的首次正常同步完成后确认新固件。</p>
    <label for="firmware">固件文件（.bin）</label>
    <input id="firmware" type="file" accept=".bin,application/octet-stream">
    <label for="checksum">校验文件（.sha256）</label>
    <input id="checksum" type="file" accept=".sha256,.txt,text/plain">
    <button id="upload" type="button">校验并更新</button>
    <div id="status" class="status"></div>
  </div>
  <script>
    const button=document.getElementById('upload');
    const statusBox=document.getElementById('status');
    button.onclick=async()=>{
      const firmware=document.getElementById('firmware').files[0];
      const checksum=document.getElementById('checksum').files[0];
      if(!firmware||!checksum){statusBox.textContent='请同时选择固件和校验文件。';return;}
      const checksumText=await checksum.text();
      const match=checksumText.match(/[0-9a-fA-F]{64}/);
      if(!match){statusBox.textContent='校验文件中没有有效的 SHA-256。';return;}
      button.disabled=true;
      statusBox.textContent='正在上传，请勿断电……';
      const form=new FormData();
      form.append('firmware',firmware,firmware.name);
      try{
        const response=await fetch('/update',{method:'POST',headers:{'X-Firmware-SHA256':match[0].toLowerCase()},body:form});
        const text=await response.text();
        statusBox.textContent=text;
        if(!response.ok)button.disabled=false;
      }catch(error){statusBox.textContent='上传中断：'+error;button.disabled=false;}
    };
  </script>
</body>
</html>
)OTAHTML";

static bool otaPasswordConfigured()
{
#ifdef EINK_OTA_PASSWORD
  return strlen(EINK_OTA_PASSWORD) >= 12 &&
         strstr(EINK_OTA_PASSWORD, "REPLACE_") == nullptr;
#else
  return false;
#endif
}

static bool otaRequestAuthenticated()
{
#ifdef EINK_OTA_PASSWORD
  if (otaServer.authenticate(OTA_USERNAME, EINK_OTA_PASSWORD)) return true;
#endif
  otaServer.requestAuthentication(DIGEST_AUTH, "E-Ink Calendar OTA",
                                  "需要 OTA 维护密码");
  return false;
}

static bool isSha256Hex(const String& value)
{
  if (value.length() != 64) return false;
  for (size_t index = 0; index < 64; ++index)
  {
    if (hexNibble(value[index]) < 0) return false;
  }
  return true;
}

static void abortOtaUpload(const String& message)
{
  if (otaUploadStarted)
  {
    Update.abort();
    mbedtls_sha256_free(&otaSha256Context);
  }
  otaUploadStarted = false;
  otaUploadSucceeded = false;
  otaUploadError = message;
  Serial.print("OTA upload aborted: ");
  Serial.println(message);
}

static void handleOtaUploadChunk()
{
  HTTPUpload& upload = otaServer.upload();
  if (upload.status == UPLOAD_FILE_START)
  {
    otaUploadAuthorized = otaServer.authenticate(OTA_USERNAME, EINK_OTA_PASSWORD);
    otaUploadStarted = false;
    otaUploadSucceeded = false;
    otaUploadError = "";
    memset(otaExpectedSha256, 0, sizeof(otaExpectedSha256));
    if (!otaUploadAuthorized)
    {
      otaUploadError = "身份验证失败";
      return;
    }

    String expectedHash = otaServer.header("X-Firmware-SHA256");
    expectedHash.trim();
    expectedHash.toLowerCase();
    if (!isSha256Hex(expectedHash))
    {
      otaUploadError = "缺少有效的 SHA-256 校验值";
      return;
    }
    strlcpy(otaExpectedSha256, expectedHash.c_str(), sizeof(otaExpectedSha256));

    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH))
    {
      otaUploadError = String("无法开始更新：") + Update.errorString();
      return;
    }
    mbedtls_sha256_init(&otaSha256Context);
    if (mbedtls_sha256_starts_ret(&otaSha256Context, 0) != 0)
    {
      Update.abort();
      mbedtls_sha256_free(&otaSha256Context);
      otaUploadError = "无法初始化 SHA-256 校验";
      return;
    }
    otaUploadStarted = true;
    Serial.printf("OTA upload started: %s.\n", upload.filename.c_str());
    return;
  }

  if (!otaUploadAuthorized || !otaUploadStarted) return;

  if (upload.status == UPLOAD_FILE_WRITE)
  {
    if (mbedtls_sha256_update_ret(&otaSha256Context, upload.buf,
                                  upload.currentSize) != 0)
    {
      abortOtaUpload("SHA-256 计算失败");
      return;
    }
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
    {
      abortOtaUpload(String("固件写入失败：") + Update.errorString());
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_END)
  {
    uint8_t digest[32] = {0};
    const int hashStatus = mbedtls_sha256_finish_ret(&otaSha256Context, digest);
    mbedtls_sha256_free(&otaSha256Context);
    otaUploadStarted = false;
    if (hashStatus != 0)
    {
      Update.abort();
      otaUploadError = "SHA-256 计算失败";
      return;
    }

    char actualHash[65] = {0};
    encodeHex(digest, sizeof(digest), actualHash);
    memset(digest, 0, sizeof(digest));
    if (strcasecmp(actualHash, otaExpectedSha256) != 0)
    {
      Update.abort();
      otaUploadError = "SHA-256 不匹配，固件已拒绝";
      Serial.println("OTA SHA-256 mismatch; candidate rejected.");
      return;
    }

    if (!Update.end(true))
    {
      otaUploadError = String("固件校验失败：") + Update.errorString();
      return;
    }
    otaUploadSucceeded = true;
    Serial.printf("OTA image accepted: %u bytes, SHA-256 verified.\n",
                  upload.totalSize);
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED)
  {
    abortOtaUpload("上传连接已中断");
  }
}

static bool runningFirmwareNeedsVerification()
{
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  return running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
         state == ESP_OTA_IMG_PENDING_VERIFY;
}

static void confirmRunningFirmware()
{
  if (!firmwarePendingVerification) return;
  const esp_err_t status = esp_ota_mark_app_valid_cancel_rollback();
  if (status == ESP_OK)
  {
    firmwarePendingVerification = false;
    Serial.println("OTA firmware verified after successful sync; rollback cancelled.");
  }
  else
  {
    Serial.printf("Unable to confirm OTA firmware: %s.\n", esp_err_to_name(status));
  }
}

static void rollbackUnverifiedFirmware()
{
  if (!firmwarePendingVerification) return;
  Serial.println("New OTA firmware did not complete a successful sync; rolling back.");
  Serial.flush();
  delay(50);
  if (esp_ota_check_rollback_is_possible())
  {
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
  Serial.println("Rollback partition unavailable; retaining current firmware.");
}

static void runOtaMaintenanceWindow()
{
  if (!otaPasswordConfigured())
  {
    Serial.println("OTA maintenance disabled: configure EINK_OTA_PASSWORD.");
    return;
  }
  if (WiFi.status() != WL_CONNECTED || firmwarePendingVerification) return;

  otaUploadAuthorized = false;
  otaUploadStarted = false;
  otaUploadSucceeded = false;
  otaRestartRequested = false;
  otaUploadError = "";

  static const char* HEADER_KEYS[] = {"X-Firmware-SHA256"};
  otaServer.collectHeaders(HEADER_KEYS, 1);
  otaServer.on("/", HTTP_GET, []()
  {
    if (!otaRequestAuthenticated()) return;
    otaServer.sendHeader("Location", "/update", true);
    otaServer.send(302, "text/plain", "");
  });
  otaServer.on("/update", HTTP_GET, []()
  {
    if (!otaRequestAuthenticated()) return;
    otaServer.sendHeader("Cache-Control", "no-store");
    otaServer.send_P(200, "text/html; charset=utf-8", OTA_PAGE);
  });
  otaServer.on("/update", HTTP_POST, []()
  {
    if (!otaRequestAuthenticated()) return;
    otaServer.sendHeader("Connection", "close");
    if (otaUploadSucceeded)
    {
      otaServer.send(200, "text/plain; charset=utf-8",
                     "更新校验通过，设备正在重启。新版本会在首次成功同步后自动确认。");
      otaRestartRequested = true;
    }
    else
    {
      const String message = otaUploadError.length() > 0 ?
        otaUploadError : "固件更新失败";
      otaServer.send(400, "text/plain; charset=utf-8", message);
    }
  }, handleOtaUploadChunk);
  otaServer.onNotFound([]()
  {
    otaServer.send(404, "text/plain; charset=utf-8", "页面不存在");
  });

  const bool mdnsStarted = MDNS.begin(OTA_HOSTNAME);
  if (mdnsStarted) MDNS.addService("http", "tcp", 80);
  otaServer.begin();
  Serial.printf("OTA maintenance window open for %lu seconds.\n",
                static_cast<unsigned long>(OTA_WINDOW_MS / 1000UL));
  Serial.printf("Open http://%s.local/update or http://%s/update; user=%s.\n",
                OTA_HOSTNAME, WiFi.localIP().toString().c_str(), OTA_USERNAME);

  const unsigned long startedAt = millis();
  while (!otaRestartRequested && millis() - startedAt < OTA_WINDOW_MS + OTA_UPLOAD_GRACE_MS)
  {
    if (!otaUploadStarted && millis() - startedAt >= OTA_WINDOW_MS) break;
    otaServer.handleClient();
    delay(2);
  }
  otaServer.stop();
  if (mdnsStarted) MDNS.end();

  if (otaRestartRequested)
  {
    Serial.println("Restarting into the verified OTA candidate...");
    Serial.flush();
    delay(500);
    ESP.restart();
  }
  Serial.println("OTA maintenance window closed.");
}

static const char* wakeCauseText(esp_sleep_wakeup_cause_t cause)
{
  switch (cause)
  {
    case ESP_SLEEP_WAKEUP_TIMER: return "timer";
    case ESP_SLEEP_WAKEUP_EXT0: return "external pin 0";
    case ESP_SLEEP_WAKEUP_EXT1: return "external pin 1";
    case ESP_SLEEP_WAKEUP_TOUCHPAD: return "touch";
    case ESP_SLEEP_WAKEUP_ULP: return "ULP";
    default: return "power-on/reset";
  }
}

static void releaseDisplayPinHolds()
{
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis(static_cast<gpio_num_t>(EPD_PWR));
  gpio_hold_dis(static_cast<gpio_num_t>(EPD_RST));
  gpio_hold_dis(static_cast<gpio_num_t>(EPD_CS));

  pinMode(EPD_PWR, OUTPUT);
  digitalWrite(EPD_PWR, HIGH);
  pinMode(EPD_RST, OUTPUT);
  digitalWrite(EPD_RST, HIGH);
  pinMode(EPD_CS, OUTPUT);
  digitalWrite(EPD_CS, HIGH);
}

static void holdDisplayControllerForSleep()
{
  // The panel drive voltages are already off. Keep only the controller supply
  // and safe control levels so GxEPD2 can resume with initial=false and perform
  // a genuine partial update after processor deep sleep.
  pinMode(EPD_PWR, OUTPUT);
  digitalWrite(EPD_PWR, HIGH);
  pinMode(EPD_RST, OUTPUT);
  digitalWrite(EPD_RST, HIGH);
  pinMode(EPD_CS, OUTPUT);
  digitalWrite(EPD_CS, HIGH);
  delay(2);

  gpio_hold_en(static_cast<gpio_num_t>(EPD_PWR));
  gpio_hold_en(static_cast<gpio_num_t>(EPD_RST));
  gpio_hold_en(static_cast<gpio_num_t>(EPD_CS));
  gpio_deep_sleep_hold_en();
}

static uint32_t secondsUntilNextRegularWake(time_t now)
{
  if (now < 1700000000) return WAKE_INTERVAL_SECONDS;

  const int64_t interval = WAKE_INTERVAL_SECONDS;
  int64_t wakeAt = (static_cast<int64_t>(now) / interval + 1) * interval +
                   WAKE_BOUNDARY_OFFSET_SECONDS;
  int64_t delaySeconds = wakeAt - static_cast<int64_t>(now);
  if (delaySeconds < MINIMUM_REGULAR_SLEEP_SECONDS) delaySeconds += interval;
  return static_cast<uint32_t>(delaySeconds);
}

static void enterDeepSleep(uint32_t sleepSeconds)
{
  if (sleepSeconds < 30) sleepSeconds = 30;

  // Keep an estimated wall clock in RTC state. ESP32's system clock is not
  // guaranteed to survive a cold reset, but this lets the stale-data timer
  // continue advancing through Wi-Fi/NTP outages after at least one sync.
  const uint32_t systemEpoch = systemEpochOrZero();
  if (systemEpoch > 0)
  {
    const uint64_t predictedWake =
      static_cast<uint64_t>(systemEpoch) + sleepSeconds;
    rtcState.lastKnownEpoch = predictedWake > UINT32_MAX ?
      UINT32_MAX : static_cast<uint32_t>(predictedWake);
  }
  else if (rtcState.lastKnownEpoch > 0)
  {
    const uint64_t predictedWake =
      static_cast<uint64_t>(rtcState.lastKnownEpoch) + sleepSeconds;
    rtcState.lastKnownEpoch = predictedWake > UINT32_MAX ?
      UINT32_MAX : static_cast<uint32_t>(predictedWake);
  }
  persistRtcState();

  if (displayInitialized)
  {
    display.powerOff();
    SPI.end();
  }

  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  holdDisplayControllerForSleep();

  const time_t now = time(nullptr);
  if (isValidUnixEpoch(now))
  {
    const time_t wakeAt = now + sleepSeconds;
    tm wakeTime = {};
    localtime_r(&wakeAt, &wakeTime);
    Serial.printf("Deep Sleep for %lu seconds; next wake around %02d:%02d:%02d.\n",
                  static_cast<unsigned long>(sleepSeconds),
                  wakeTime.tm_hour, wakeTime.tm_min, wakeTime.tm_sec);
  }
  else
  {
    Serial.printf("Deep Sleep for %lu seconds.\n",
                  static_cast<unsigned long>(sleepSeconds));
  }
  Serial.println("Wi-Fi off; e-paper drive voltage off; controller memory retained.");
  Serial.flush();
  delay(50);

  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_ON);
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(sleepSeconds) * 1000000ULL);
  esp_deep_sleep_start();
}

void setup()
{
  releaseDisplayPinHolds();
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("Starting Google-synced e-ink calendar...");
  Serial.print("Firmware: ");
  Serial.println(FIRMWARE_VERSION);
  configureLocalTimezone();
  firmwarePendingVerification = runningFirmwareNeedsVerification();
  const bool verifyingOtaFirmwareThisBoot = firmwarePendingVerification;
  if (firmwarePendingVerification)
  {
    Serial.println("First boot of an OTA candidate; successful sync is required.");
  }

  const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  const esp_reset_reason_t resetReason = esp_reset_reason();
  const bool savedStateValid = rtcStateIsValid();
  if (!savedStateValid) resetRtcState();

  const bool timerWake = wakeCause == ESP_SLEEP_WAKEUP_TIMER;
  const bool retainedScreen = timerWake && savedStateValid && rtcState.screenValid;
  displayNeedsInitialRefresh = !retainedScreen;

  if (savedStateValid)
  {
    agenda = rtcState.agenda;
    lastRenderedDate = rtcState.renderedDate;
  }
  else
  {
    memset(&agenda, 0, sizeof(agenda));
    lastRenderedDate = -1;
    uint32_t persistentSavedAt = 0;
    if (loadPersistentAgendaCache(agenda, persistentSavedAt))
    {
      rtcState.agenda = agenda;
      rtcState.lastSuccessSyncEpoch = persistentSavedAt;
      rtcState.lastKnownEpoch = persistentSavedAt;
      rtcState.lastPersistentSaveEpoch = persistentSavedAt;
      rtcState.hasSuccessfulSync = 1;
      Serial.println("Cold-start fallback data is ready if the network is unavailable.");
    }
  }

  ++rtcState.bootCount;
  Serial.printf("Wake #%lu: %s; RTC state %s; display memory %s.\n",
                static_cast<unsigned long>(rtcState.bootCount),
                wakeCauseText(wakeCause),
                savedStateValid ? "valid" : "new",
                retainedScreen ? "retained" : "requires full refresh");
  Serial.printf("Reset reason: %d.\n", static_cast<int>(resetReason));

  const bool manualReset = resetReason == ESP_RST_EXT;
  const bool allowProvisioningPortal = !retainedScreen &&
    (!agenda.valid || manualReset || rtcState.consecutiveFailCount >= 2);
  // Timer wakes stay fast; only a reset or power-on waits for the request.
  const bool forcePortal = !timerWake && serialPortalRequested(3000);
  if (!connectWiFi(allowProvisioningPortal, forcePortal))
  {
    Serial.println("Wi-Fi setup failed.");
    recordSyncDiagnostics(SyncDiagnostics());
    const bool healthChanged = recordSyncOutcome(
      false, SyncFailureReason::WiFi, healthNowEpoch());
    if (provisioningPageShown)
    {
      rtcState.screenValid = 0;
      lastRenderedDate = -1;
    }
    if (!rtcState.screenValid && !agenda.valid)
    {
      renderFullPage(drawWiFiErrorPage);
    }
    else if (!rtcState.screenValid)
    {
      Serial.println("Cold-start cache exists; preserving the last e-paper image until time is available.");
    }
    else
    {
      tm healthTime = {};
      if (healthEpochToLocalTime(healthTime))
      {
        renderHealthTransition(healthTime, healthChanged);
      }
      else if (healthChanged)
      {
        Serial.println("Health state changed, but no valid local clock for header refresh.");
      }
    }
    rollbackUnverifiedFirmware();
    enterDeepSleep(retrySleepSeconds(rtcState.consecutiveFailCount));
  }

  tm currentTime = {};
  if (!synchronizeClock(currentTime))
  {
    Serial.println("Network time unavailable.");
    recordSyncDiagnostics(SyncDiagnostics());
    const bool healthChanged = recordSyncOutcome(
      false, SyncFailureReason::Ntp, healthNowEpoch());
    if (!rtcState.screenValid && !agenda.valid)
    {
      renderFullPage(drawClockErrorPage);
    }
    else if (!rtcState.screenValid)
    {
      Serial.println("Cold-start cache exists; preserving the last e-paper image until time is available.");
    }
    else
    {
      tm healthTime = {};
      if (healthEpochToLocalTime(healthTime))
      {
        renderHealthTransition(healthTime, healthChanged);
      }
      else if (healthChanged)
      {
        Serial.println("Health state changed, but no valid local clock for header refresh.");
      }
    }
    rollbackUnverifiedFirmware();
    enterDeepSleep(retrySleepSeconds(rtcState.consecutiveFailCount));
  }

  const uint32_t healthEpoch = healthNowEpoch();
  ensureInitialHealthReference(healthEpoch);
  if (agenda.valid && !agendaMatchesLocalDate(agenda, currentTime))
  {
    Serial.printf("Cached agenda is for %s, not today; keeping it in NVS but not displaying it.\n",
                  agenda.date);
    memset(&agenda, 0, sizeof(agenda));
    rtcState.agenda = agenda;
  }

  // Static storage avoids reserving the full agenda payload on loopTask's stack.
  static AgendaData freshAgenda;
  memset(&freshAgenda, 0, sizeof(freshAgenda));
  SyncFailureReason failureReason = SyncFailureReason::None;
  SyncDiagnostics diagnostics = {};
  const bool syncSucceeded = fetchGoogleAgendaWithRetry(
    currentTime, freshAgenda, failureReason, diagnostics);
  recordSyncDiagnostics(diagnostics);
  const bool healthChanged = recordSyncOutcome(
    syncSucceeded, failureReason, healthNowEpoch());

  const time_t now = time(nullptr);
  const int32_t currentDate = dateKey(currentTime);
  const bool reusableDashboard = retainedScreen && lastRenderedDate == currentDate;
  bool fullRefreshRequired = !reusableDashboard;
  if (rtcState.lastFullRefreshEpoch <= 0 ||
      now < rtcState.lastFullRefreshEpoch ||
      now - rtcState.lastFullRefreshEpoch >= FULL_REFRESH_INTERVAL_SECONDS)
  {
    fullRefreshRequired = true;
    Serial.println("Full refresh due: daily refresh interval reached.");
  }

  bool persistentCacheHealthy = true;
  if (syncSucceeded)
  {
    const bool agendaChanged = !rtcState.screenValid || !sameAgenda(agenda, freshAgenda);
    agenda = freshAgenda;
    persistentCacheHealthy = savePersistentAgendaCache(
      agenda, healthNowEpoch());

    // A scheduled sync must always be visibly reflected on the e-paper
    // display. Full refresh is deliberately used instead of a partial update:
    // the controller RAM may not survive every deep-sleep wake on this HAT.
    if (timerWake && FORCE_VISIBLE_REFRESH_ON_TIMER_SYNC)
    {
      fullRefreshRequired = true;
      Serial.println("Full refresh due: scheduled visible-refresh policy.");
    }
    else if (timerWake && (agendaChanged || healthChanged))
    {
      fullRefreshRequired = true;
      Serial.println("Full refresh due: visible data changed after deep sleep.");
    }

    if ((agendaChanged || healthChanged) &&
        rtcState.partialRefreshCount >= MAX_PARTIAL_REFRESHES)
    {
      fullRefreshRequired = true;
      Serial.println("Full refresh due: partial refresh limit reached.");
    }

    if (fullRefreshRequired)
    {
      renderDashboard(currentTime);
      rtcState.lastFullRefreshEpoch = now;
      rtcState.partialRefreshCount = 0;
      rtcState.screenValid = 1;
    }
    else if (agendaChanged || healthChanged)
    {
      shownTime = currentTime;
      renderPartialUpdates(healthChanged, agendaChanged);
      ++rtcState.partialRefreshCount;
      Serial.printf("Partial refresh count: %u/%u.\n",
                    rtcState.partialRefreshCount, MAX_PARTIAL_REFRESHES);
    }
    else
    {
      Serial.println("Google data unchanged; e-paper refresh skipped.");
    }
  }
  else if (fullRefreshRequired)
  {
    if (agendaMatchesLocalDate(agenda, currentTime))
    {
      Serial.println("Sync failed; rebuilding the dashboard from today's persistent cache.");
    }
    else
    {
      Serial.println("Sync failed and no current-date cache is available.");
      memset(&agenda, 0, sizeof(agenda));
    }
    renderDashboard(currentTime);
    rtcState.lastFullRefreshEpoch = now;
    rtcState.partialRefreshCount = 0;
    rtcState.screenValid = 1;
  }
  else
  {
    if (timerWake && healthChanged)
    {
      // Apply the same safe fallback to a newly visible health warning or
      // recovery state after a timer wake.
      Serial.println("Full refresh due: health state changed after deep sleep.");
      renderDashboard(currentTime);
      rtcState.lastFullRefreshEpoch = now;
      rtcState.partialRefreshCount = 0;
      rtcState.screenValid = 1;
    }
    else
    {
      Serial.println("Sync failed; preserving the last valid e-paper image.");
      renderHealthTransition(currentTime, healthChanged);
    }
  }

  if (firmwarePendingVerification)
  {
    if (syncSucceeded && persistentCacheHealthy)
    {
      confirmRunningFirmware();
    }
    else
    {
      rollbackUnverifiedFirmware();
    }
  }

  if (!timerWake && syncSucceeded && !verifyingOtaFirmwareThisBoot)
  {
    persistRtcState();
    if (displayInitialized)
    {
      display.powerOff();
      SPI.end();
      displayInitialized = false;
    }
    runOtaMaintenanceWindow();
  }

  const uint32_t nextSleepSeconds = syncSucceeded ?
    secondsUntilNextRegularWake(time(nullptr)) :
    retrySleepSeconds(rtcState.consecutiveFailCount);
  enterDeepSleep(nextSleepSeconds);
}

void loop()
{
  // All work is performed once in setup(), followed by timer Deep Sleep.
}
