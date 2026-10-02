const JSON_HEADERS = {
  "content-type": "application/json; charset=utf-8",
  "cache-control": "no-store, max-age=0",
  "x-content-type-options": "nosniff",
};

const MAX_CLOCK_SKEW_SECONDS = 300;

// Google Apps Script sometimes needs 30-60 seconds or fails with a transient
// 404, while the device gives up after 20 seconds. A Cron Trigger refreshes a
// KV snapshot in the background, so device requests are normally answered
// from KV without waiting for Google.
const SNAPSHOT_KEY = "dashboard-snapshot-v1";
const SNAPSHOT_TTL_SECONDS = 24 * 60 * 60;
const FRESH_SNAPSHOT_MS = 20 * 60 * 1000;
// Matches the device's stale-data warning threshold, so a prolonged Google
// outage still becomes visible on the e-paper health indicator.
const MAX_STALE_SNAPSHOT_MS = 2 * 60 * 60 * 1000;
// Leaves room for authentication, KV and encryption inside the device's
// 20-second HTTP timeout.
const LIVE_UPSTREAM_TIMEOUT_MS = 12000;
const SCHEDULED_UPSTREAM_TIMEOUT_MS = 50000;
const SCHEDULED_UPSTREAM_ATTEMPTS = 2;
const DATA_TIME_ZONE = "Asia/Shanghai";
const encoder = new TextEncoder();

function jsonResponse(payload, status = 200, extraHeaders = {}) {
  return new Response(JSON.stringify(payload), {
    status,
    headers: { ...JSON_HEADERS, ...extraHeaders },
  });
}

function hexToBytes(hex) {
  const bytes = new Uint8Array(hex.length / 2);
  for (let index = 0; index < bytes.length; index += 1) {
    bytes[index] = Number.parseInt(hex.slice(index * 2, index * 2 + 2), 16);
  }
  return bytes;
}

function bytesToBase64(bytes) {
  let binary = "";
  for (let offset = 0; offset < bytes.length; offset += 0x8000) {
    binary += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000));
  }
  return btoa(binary);
}

async function deriveSubkey(secretBytes, label) {
  const rootKey = await crypto.subtle.importKey(
    "raw",
    secretBytes,
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["sign"],
  );
  return new Uint8Array(await crypto.subtle.sign("HMAC", rootKey, encoder.encode(label)));
}

async function authenticateRequest(request, authKeyBytes) {
  const requestUrl = new URL(request.url);
  const timestamp = request.headers.get("x-device-time") || "";
  const nonce = (request.headers.get("x-device-nonce") || "").toLowerCase();
  const signatureHex = (request.headers.get("x-device-signature") || "").toLowerCase();

  if (!/^\d{10}$/.test(timestamp) ||
      !/^[0-9a-f]{32}$/.test(nonce) ||
      !/^[0-9a-f]{64}$/.test(signatureHex)) {
    return null;
  }

  const requestSeconds = Number(timestamp);
  const serverSeconds = Math.floor(Date.now() / 1000);
  if (!Number.isSafeInteger(requestSeconds) ||
      Math.abs(serverSeconds - requestSeconds) > MAX_CLOCK_SKEW_SECONDS) {
    return null;
  }

  const message = `GET\n${requestUrl.pathname}\n${timestamp}\n${nonce}`;
  const hmacKey = await crypto.subtle.importKey(
    "raw",
    authKeyBytes,
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["verify"],
  );
  const authenticated = await crypto.subtle.verify(
    "HMAC",
    hmacKey,
    hexToBytes(signatureHex),
    encoder.encode(message),
  );

  return authenticated ? { timestamp, nonce } : null;
}

async function encryptPayload(payload, requestAuth, encryptionKeyBytes) {
  const aesKey = await crypto.subtle.importKey(
    "raw",
    encryptionKeyBytes,
    { name: "AES-GCM" },
    false,
    ["encrypt"],
  );
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const aad = encoder.encode(`v1\n${requestAuth.timestamp}\n${requestAuth.nonce}`);
  const sealed = new Uint8Array(await crypto.subtle.encrypt(
    { name: "AES-GCM", iv, additionalData: aad, tagLength: 128 },
    aesKey,
    encoder.encode(JSON.stringify(payload)),
  ));

  return {
    v: 1,
    t: Number(requestAuth.timestamp),
    n: requestAuth.nonce,
    iv: bytesToBase64(iv),
    data: bytesToBase64(sealed),
  };
}

function todayInDataTimeZone(now = new Date()) {
  // en-CA formats as YYYY-MM-DD, matching the Apps Script "date" field.
  return new Intl.DateTimeFormat("en-CA", {
    timeZone: DATA_TIME_ZONE,
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
  }).format(now);
}

async function fetchUpstreamPayload(env, timeoutMs) {
  const upstreamUrl = new URL(env.GOOGLE_API_URL);
  upstreamUrl.searchParams.set("key", env.DEVICE_KEY);
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const upstream = await fetch(upstreamUrl.toString(), {
      method: "GET",
      headers: {
        accept: "application/json",
        "user-agent": "EInkCalendarRelay/2.1",
      },
      redirect: "follow",
      signal: controller.signal,
      cf: { cacheTtl: 0, cacheEverything: false },
    });
    if (!upstream.ok) {
      return { error: "upstream_failure", status: upstream.status };
    }
    const payload = await upstream.json();
    if (payload?.ok !== true || typeof payload.date !== "string") {
      return { error: "invalid_upstream_payload" };
    }
    return { payload };
  } catch (error) {
    return { error: error?.name === "AbortError" ? "upstream_timeout" : "relay_request_failed" };
  } finally {
    clearTimeout(timer);
  }
}

async function readSnapshot(env) {
  if (!env.EINK_CACHE) return null;
  try {
    const snapshot = await env.EINK_CACHE.get(SNAPSHOT_KEY, { type: "json" });
    if (!snapshot || snapshot.payload?.ok !== true ||
        !Number.isFinite(snapshot.fetchedAt)) {
      return null;
    }
    return snapshot;
  } catch {
    return null;
  }
}

async function writeSnapshot(env, payload) {
  if (!env.EINK_CACHE) return;
  await env.EINK_CACHE.put(
    SNAPSHOT_KEY,
    JSON.stringify({ fetchedAt: Date.now(), payload }),
    { expirationTtl: SNAPSHOT_TTL_SECONDS },
  );
}

function snapshotUsable(snapshot, maxAgeMs, now = Date.now()) {
  return Boolean(snapshot) &&
    snapshot.payload.date === todayInDataTimeZone(new Date(now)) &&
    now - snapshot.fetchedAt >= 0 &&
    now - snapshot.fetchedAt <= maxAgeMs;
}

async function refreshSnapshot(env) {
  /** @type {Awaited<ReturnType<typeof fetchUpstreamPayload>>} */
  let result = { error: "not_attempted" };
  for (let attempt = 1; attempt <= SCHEDULED_UPSTREAM_ATTEMPTS; attempt += 1) {
    result = await fetchUpstreamPayload(env, SCHEDULED_UPSTREAM_TIMEOUT_MS);
    if (result.payload) {
      await writeSnapshot(env, result.payload);
      console.log(`snapshot refreshed on attempt ${attempt} for ${result.payload.date}`);
      return result;
    }
    console.warn(`snapshot refresh attempt ${attempt} failed: ${result.error} ${result.status ?? ""}`);
  }
  return result;
}

async function resolvePayload(env, ctx) {
  const snapshot = await readSnapshot(env);
  if (snapshotUsable(snapshot, FRESH_SNAPSHOT_MS)) {
    return { payload: snapshot.payload, source: "kv" };
  }

  const live = await fetchUpstreamPayload(env, LIVE_UPSTREAM_TIMEOUT_MS);
  if (live.payload) {
    if (env.EINK_CACHE) ctx.waitUntil(writeSnapshot(env, live.payload).catch(() => {}));
    return { payload: live.payload, source: "live" };
  }

  if (snapshotUsable(snapshot, MAX_STALE_SNAPSHOT_MS)) {
    return { payload: snapshot.payload, source: "stale" };
  }
  return live;
}

export default {
  async fetch(request, env, ctx) {
    if (request.method !== "GET") {
      return jsonResponse({ ok: false, error: "method_not_allowed" }, 405, { allow: "GET" });
    }

    if (!/^[0-9a-fA-F]{64}$/.test(env.DEVICE_KEY || "") || !env.GOOGLE_API_URL) {
      return jsonResponse({ ok: false, error: "relay_not_configured" }, 503);
    }

    const secretBytes = hexToBytes(env.DEVICE_KEY);
    const authKeyBytes = await deriveSubkey(secretBytes, "eink-calendar-auth-v1");
    const encryptionKeyBytes = await deriveSubkey(secretBytes, "eink-calendar-encryption-v1");
    const requestAuth = await authenticateRequest(request, authKeyBytes);
    if (!requestAuth) {
      return jsonResponse({ ok: false, error: "unauthorized" }, 401);
    }

    const result = await resolvePayload(env, ctx);
    if (!result.payload) {
      const failure = { ok: false, error: result.error };
      if (result.status) failure.status = result.status;
      return jsonResponse(failure, 502);
    }

    return jsonResponse(await encryptPayload(result.payload, requestAuth, encryptionKeyBytes), 200, {
      "x-eink-crypto": "aes-256-gcm",
      "x-eink-source": result.source,
    });
  },

  async scheduled(event, env, ctx) {
    if (!/^[0-9a-fA-F]{64}$/.test(env.DEVICE_KEY || "") || !env.GOOGLE_API_URL ||
        !env.EINK_CACHE) {
      console.warn("scheduled refresh skipped: DEVICE_KEY, GOOGLE_API_URL or EINK_CACHE missing");
      return;
    }
    ctx.waitUntil(refreshSnapshot(env));
  },
};
