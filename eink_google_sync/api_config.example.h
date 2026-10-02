#pragma once

// Use at least 16 random characters. The OTA page is available only for two
// minutes after a manual reset or power-on.
#define EINK_OTA_PASSWORD "REPLACE_WITH_RANDOM_OTA_PASSWORD"

// Health-state display thresholds. Change these values to suit your tolerance
// for stale Calendar / Tasks data. Values are in seconds.
#define EINK_HEALTH_WARNING_STALE_SECONDS (2UL * 60UL * 60UL)
#define EINK_HEALTH_CRITICAL_STALE_SECONDS (12UL * 60UL * 60UL)

static constexpr char GOOGLE_API_URL[] =
  "http://eink-calendar-relay.YOUR_SUBDOMAIN.workers.dev/";
static constexpr char GOOGLE_API_KEY[] = "YOUR_RANDOM_DEVICE_KEY";

// Use the same hostname in RELAY_HOST in the sketch. GOOGLE_API_URL points
// to the encrypted Worker relay, not directly to the Apps Script endpoint.

// Optional LAN relay (e.g. nas_relay/eink_nas_relay.py on a NAS). The device
// falls back to GOOGLE_API_URL when it is unreachable. Leave empty to disable.
#define EINK_LOCAL_RELAY_URL ""
