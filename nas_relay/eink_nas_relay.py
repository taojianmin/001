#!/usr/bin/env python3
"""LAN relay for the e-ink calendar, meant to run on the Synology NAS.

The e-ink device's network drops requests to *.workers.dev at random and
serves polluted DNS for it. This service sits on the home LAN instead:

* A background thread fetches the dashboard from the Cloudflare Worker every
  REFRESH_SECONDS, using the same HMAC-signed / AES-256-GCM protocol as the
  device, and keeps the decrypted payload in memory and on disk.
* The device sends its usual signed GET to this service and receives the
  cached payload, encrypted for its own timestamp and nonce, exactly as the
  Worker would answer. The firmware therefore needs only a different URL.

Standard library only (Python 3.8+), so it runs on DSM without extra
packages. AES-GCM is implemented below because the stdlib has no AES.

Configuration comes from environment variables or a KEY=VALUE file passed
with --config (see eink_nas_relay.env.example).
"""

import argparse
import base64
import hashlib
import hmac
import http.client
import json
import os
import socket
import sys
import threading
import time
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DATA_TIME_ZONE = timezone(timedelta(hours=8))  # Asia/Shanghai, no DST
MAX_CLOCK_SKEW_SECONDS = 300
FRESH_SNAPSHOT_SECONDS = 20 * 60
# Matches the device's stale-data warning threshold, so a prolonged outage
# still shows up on the e-paper health indicator.
MAX_STALE_SNAPSHOT_SECONDS = 2 * 60 * 60
# Per upstream address; a dropped request never answers, so move on quickly.
LIVE_FETCH_TIMEOUT_SECONDS = 3
BACKGROUND_FETCH_TIMEOUT_SECONDS = 12


# --------------------------------------------------------------------------
# AES-256 (encryption direction only; GCM never needs the inverse cipher)
# --------------------------------------------------------------------------

def _build_sbox():
    sbox = [0] * 256
    p = q = 1
    while True:
        # p *= 3 in GF(2^8)
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        # q /= 3 in GF(2^8)
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q ^ _rotl8(q, 1) ^ _rotl8(q, 2) ^ _rotl8(q, 3) ^ _rotl8(q, 4)
        sbox[p] = x ^ 0x63
        if p == 1:
            break
    sbox[0] = 0x63
    return sbox


def _rotl8(x, shift):
    return ((x << shift) | (x >> (8 - shift))) & 0xFF


_SBOX = _build_sbox()


def _xtime(a):
    a <<= 1
    return (a ^ 0x1B) & 0xFF if a & 0x100 else a


class AES256:
    def __init__(self, key):
        if len(key) != 32:
            raise ValueError("AES-256 key must be 32 bytes")
        self._round_keys = self._expand(key)

    @staticmethod
    def _expand(key):
        words = [list(key[i:i + 4]) for i in range(0, 32, 4)]
        rcon = 1
        for i in range(8, 60):
            temp = list(words[i - 1])
            if i % 8 == 0:
                temp = temp[1:] + temp[:1]
                temp = [_SBOX[b] for b in temp]
                temp[0] ^= rcon
                rcon = _xtime(rcon)
            elif i % 8 == 4:
                temp = [_SBOX[b] for b in temp]
            words.append([words[i - 8][j] ^ temp[j] for j in range(4)])
        return [sum(words[r * 4:r * 4 + 4], []) for r in range(15)]

    def encrypt_block(self, block):
        s = [b ^ k for b, k in zip(block, self._round_keys[0])]
        for rnd in range(1, 15):
            s = [_SBOX[b] for b in s]
            # ShiftRows (state is column-major: index = col * 4 + row)
            s = [s[(i + 4 * (i % 4)) % 16] for i in range(16)]
            if rnd != 14:
                mixed = []
                for c in range(4):
                    a0, a1, a2, a3 = s[c * 4:c * 4 + 4]
                    t = a0 ^ a1 ^ a2 ^ a3
                    mixed += [
                        a0 ^ t ^ _xtime(a0 ^ a1),
                        a1 ^ t ^ _xtime(a1 ^ a2),
                        a2 ^ t ^ _xtime(a2 ^ a3),
                        a3 ^ t ^ _xtime(a3 ^ a0),
                    ]
                s = mixed
            s = [b ^ k for b, k in zip(s, self._round_keys[rnd])]
        return bytes(s)


# --------------------------------------------------------------------------
# GCM mode (96-bit IV, 128-bit tag)
# --------------------------------------------------------------------------

_GCM_R = 0xE1 << 120


def _gf_mult(x, y):
    z = 0
    v = y
    for i in range(127, -1, -1):
        if (x >> i) & 1:
            z ^= v
        v = (v >> 1) ^ _GCM_R if v & 1 else v >> 1
    return z


def _ghash(h, aad, ciphertext):
    def blocks(data):
        for i in range(0, len(data), 16):
            chunk = data[i:i + 16]
            yield int.from_bytes(chunk + b"\x00" * (16 - len(chunk)), "big")

    y = 0
    for block in blocks(aad):
        y = _gf_mult(y ^ block, h)
    for block in blocks(ciphertext):
        y = _gf_mult(y ^ block, h)
    lengths = (len(aad) * 8).to_bytes(8, "big") + (len(ciphertext) * 8).to_bytes(8, "big")
    return _gf_mult(y ^ int.from_bytes(lengths, "big"), h)


def _gctr(aes, counter_block, data):
    out = bytearray()
    counter = int.from_bytes(counter_block, "big")
    for i in range(0, len(data), 16):
        keystream = aes.encrypt_block(counter.to_bytes(16, "big"))
        chunk = data[i:i + 16]
        out += bytes(a ^ b for a, b in zip(chunk, keystream))
        counter = (counter & ~0xFFFFFFFF) | ((counter + 1) & 0xFFFFFFFF)
    return bytes(out)


def gcm_encrypt(key, iv, plaintext, aad):
    aes = AES256(key)
    h = int.from_bytes(aes.encrypt_block(b"\x00" * 16), "big")
    j0 = iv + b"\x00\x00\x00\x01"
    ciphertext = _gctr(aes, (int.from_bytes(j0, "big") + 1).to_bytes(16, "big"), plaintext)
    tag = _gctr(aes, j0, _ghash(h, aad, ciphertext).to_bytes(16, "big"))
    return ciphertext + tag


def gcm_decrypt(key, iv, sealed, aad):
    if len(sealed) < 16:
        raise ValueError("ciphertext too short")
    ciphertext, tag = sealed[:-16], sealed[-16:]
    aes = AES256(key)
    h = int.from_bytes(aes.encrypt_block(b"\x00" * 16), "big")
    j0 = iv + b"\x00\x00\x00\x01"
    expected = _gctr(aes, j0, _ghash(h, aad, ciphertext).to_bytes(16, "big"))
    if not hmac.compare_digest(expected, tag):
        raise ValueError("GCM tag mismatch")
    return _gctr(aes, (int.from_bytes(j0, "big") + 1).to_bytes(16, "big"), ciphertext)


# --------------------------------------------------------------------------
# Device protocol (mirrors cloudflare_worker/worker.js and the firmware)
# --------------------------------------------------------------------------

class Protocol:
    def __init__(self, device_key_hex):
        root = bytes.fromhex(device_key_hex)
        if len(root) != 32:
            raise ValueError("DEVICE_KEY must be 64 hex characters")
        self.auth_key = hmac.new(root, b"eink-calendar-auth-v1", hashlib.sha256).digest()
        self.enc_key = hmac.new(root, b"eink-calendar-encryption-v1", hashlib.sha256).digest()

    def sign(self, timestamp, nonce, path="/"):
        message = "GET\n{}\n{}\n{}".format(path, timestamp, nonce).encode()
        return hmac.new(self.auth_key, message, hashlib.sha256).hexdigest()

    def verify(self, headers, path):
        timestamp = headers.get("x-device-time", "")
        nonce = headers.get("x-device-nonce", "").lower()
        signature = headers.get("x-device-signature", "").lower()
        if (len(timestamp) != 10 or not timestamp.isdigit() or
                len(nonce) != 32 or not _is_hex(nonce) or
                len(signature) != 64 or not _is_hex(signature)):
            return None
        if abs(int(time.time()) - int(timestamp)) > MAX_CLOCK_SKEW_SECONDS:
            return None
        if not hmac.compare_digest(self.sign(timestamp, nonce, path), signature):
            return None
        return timestamp, nonce

    def seal(self, payload, timestamp, nonce):
        iv = os.urandom(12)
        aad = "v1\n{}\n{}".format(timestamp, nonce).encode()
        sealed = gcm_encrypt(self.enc_key, iv, json.dumps(payload, ensure_ascii=False).encode(), aad)
        return {
            "v": 1,
            "t": int(timestamp),
            "n": nonce,
            "iv": base64.b64encode(iv).decode(),
            "data": base64.b64encode(sealed).decode(),
        }

    def open(self, envelope, timestamp, nonce):
        if envelope.get("v") != 1 or envelope.get("n") != nonce or envelope.get("t") != int(timestamp):
            raise ValueError("envelope does not match request")
        aad = "v1\n{}\n{}".format(timestamp, nonce).encode()
        plain = gcm_decrypt(self.enc_key, base64.b64decode(envelope["iv"]),
                            base64.b64decode(envelope["data"]), aad)
        return json.loads(plain.decode())


def _is_hex(value):
    return all(c in "0123456789abcdef" for c in value)


def today_text(now=None):
    return datetime.fromtimestamp(now or time.time(), DATA_TIME_ZONE).strftime("%Y-%m-%d")


# --------------------------------------------------------------------------
# Upstream (Cloudflare Worker) client
# --------------------------------------------------------------------------

class Upstream:
    def __init__(self, protocol, host, fallback_ips):
        self.protocol = protocol
        self.host = host
        self.fallback_ips = fallback_ips

    def fetch(self, timeout):
        """Returns (payload, error, address used).

        The home network pollutes DNS for workers.dev and drops some requests,
        so the fixed Cloudflare addresses go first and DNS is the last resort.
        """
        last_error = "no_attempt"
        for address in self.fallback_ips + [self.host]:
            timestamp = str(int(time.time()))
            nonce = os.urandom(16).hex()
            headers = {
                "Host": self.host,
                "x-device-time": timestamp,
                "x-device-nonce": nonce,
                "x-device-signature": self.protocol.sign(timestamp, nonce),
                "user-agent": "EInkNasRelay/1.0",
            }
            try:
                connection = http.client.HTTPConnection(address, 80, timeout=timeout)
                connection.request("GET", "/", headers=headers)
                response = connection.getresponse()
                body = response.read()
                connection.close()
                if response.status != 200:
                    last_error = "http_{}".format(response.status)
                    continue
                payload = self.protocol.open(json.loads(body.decode()), timestamp, nonce)
                if payload.get("ok") is not True or not isinstance(payload.get("date"), str):
                    last_error = "invalid_payload"
                    continue
                return payload, None, address
            except (OSError, ValueError, http.client.HTTPException) as error:
                last_error = "{}:{}".format(address, type(error).__name__)
        return None, last_error, None


# --------------------------------------------------------------------------
# Snapshot cache
# --------------------------------------------------------------------------

class Snapshot:
    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        self.payload = None
        self.fetched_at = 0.0
        self.last_error = None
        self._load()

    def _load(self):
        try:
            with open(self.path, "r", encoding="utf-8") as handle:
                stored = json.load(handle)
            if stored.get("payload", {}).get("ok") is True:
                self.payload = stored["payload"]
                self.fetched_at = float(stored["fetched_at"])
        except (OSError, ValueError, KeyError, TypeError):
            pass

    def store(self, payload):
        with self.lock:
            self.payload = payload
            self.fetched_at = time.time()
            self.last_error = None
            temp = self.path + ".tmp"
            try:
                with open(temp, "w", encoding="utf-8") as handle:
                    json.dump({"fetched_at": self.fetched_at, "payload": payload}, handle, ensure_ascii=False)
                os.replace(temp, self.path)
                os.chmod(self.path, 0o600)
            except OSError as error:
                log("snapshot file write failed: {}".format(error))

    def usable(self, max_age):
        with self.lock:
            now = time.time()
            if (self.payload is not None and self.payload.get("date") == today_text(now) and
                    0 <= now - self.fetched_at <= max_age):
                return self.payload
            return None

    def status(self):
        with self.lock:
            return {
                "date": self.payload.get("date") if self.payload else None,
                "age_seconds": int(time.time() - self.fetched_at) if self.payload else None,
                "last_error": self.last_error,
            }


def log(message):
    stamp = datetime.now(DATA_TIME_ZONE).strftime("%Y-%m-%d %H:%M:%S")
    print("{} {}".format(stamp, message), flush=True)


def refresher(upstream, snapshot, interval, stop):
    while not stop.is_set():
        started = time.time()
        payload, error, address = upstream.fetch(BACKGROUND_FETCH_TIMEOUT_SECONDS)
        if payload:
            snapshot.store(payload)
            log("refreshed snapshot for {} in {:.1f}s via {}".format(
                payload["date"], time.time() - started, address))
            stop.wait(interval)
        else:
            with snapshot.lock:
                snapshot.last_error = error
            log("refresh failed: {}".format(error))
            # Retry sooner after a failure; the device must not see a gap.
            stop.wait(min(interval, 30))


def make_handler(protocol, upstream, snapshot):
    class Handler(BaseHTTPRequestHandler):
        server_version = "EInkNasRelay/1.0"

        def log_message(self, fmt, *args):
            pass

        def _json(self, status, payload, extra=None):
            body = json.dumps(payload).encode()
            self.send_response(status)
            self.send_header("content-type", "application/json; charset=utf-8")
            self.send_header("cache-control", "no-store")
            self.send_header("content-length", str(len(body)))
            for key, value in (extra or {}).items():
                self.send_header(key, value)
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path == "/health":
                self._json(200, dict(ok=True, **snapshot.status()))
                return
            headers = {k.lower(): v for k, v in self.headers.items()}
            auth = protocol.verify(headers, path)
            if not auth:
                self._json(401, {"ok": False, "error": "unauthorized"})
                return

            source = "nas-cache"
            payload = snapshot.usable(FRESH_SNAPSHOT_SECONDS)
            if payload is None:
                live, error, _ = upstream.fetch(LIVE_FETCH_TIMEOUT_SECONDS)
                if live:
                    snapshot.store(live)
                    payload, source = live, "nas-live"
                else:
                    payload, source = snapshot.usable(MAX_STALE_SNAPSHOT_SECONDS), "nas-stale"
                    if payload is None:
                        log("device request failed: {}".format(error))
                        self._json(502, {"ok": False, "error": "upstream_unavailable"})
                        return
            self._json(200, protocol.seal(payload, *auth), {"x-eink-source": source})
            log("served device {} ({})".format(self.client_address[0], source))

    return Handler


def load_config(path):
    config = dict(os.environ)
    if path:
        with open(path, "r", encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    key, value = line.split("=", 1)
                    config[key.strip()] = value.strip().strip('"')
    return config


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", help="KEY=VALUE file (default: environment only)")
    args = parser.parse_args()
    config = load_config(args.config)

    protocol = Protocol(config["DEVICE_KEY"])
    upstream = Upstream(
        protocol,
        config.get("UPSTREAM_HOST", "eink-calendar-relay.YOUR_SUBDOMAIN.workers.dev"),
        [ip.strip() for ip in config.get("UPSTREAM_FALLBACK_IPS", "104.21.12.115,172.67.152.70").split(",") if ip.strip()],
    )
    here = os.path.dirname(os.path.abspath(__file__))
    snapshot = Snapshot(config.get("SNAPSHOT_FILE", os.path.join(here, "snapshot.json")))
    interval = int(config.get("REFRESH_SECONDS", "180"))
    port = int(config.get("LISTEN_PORT", "8787"))

    stop = threading.Event()
    threading.Thread(target=refresher, args=(upstream, snapshot, interval, stop), daemon=True).start()
    server = ThreadingHTTPServer(("0.0.0.0", port), make_handler(protocol, upstream, snapshot))
    log("listening on :{} (refresh every {}s)".format(port, interval))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        server.server_close()


if __name__ == "__main__":
    socket.setdefaulttimeout(30)
    sys.exit(main())
