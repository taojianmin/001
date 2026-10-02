"""End-to-end check: start the relay, let it pull from the live Worker, then
query it exactly like the device and decrypt the answer with Node's crypto
(an implementation independent of this project's pure-Python AES).

Usage: python test_end_to_end.py <path to api_config.h>
The device key is read from api_config.h and never printed.
"""
import hashlib
import hmac
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = 18787

source = open(sys.argv[1], encoding="utf-8").read()
key = re.search(r'GOOGLE_API_KEY\[\]\s*=\s*"([0-9a-f]{64})"', source).group(1)
auth_key = hmac.new(bytes.fromhex(key), b"eink-calendar-auth-v1", hashlib.sha256).digest()
enc_key = hmac.new(bytes.fromhex(key), b"eink-calendar-encryption-v1", hashlib.sha256).hexdigest()

snapshot = os.path.join(tempfile.gettempdir(), "eink_nas_relay_e2e.json")
if os.path.exists(snapshot):
    os.remove(snapshot)
env = dict(os.environ, DEVICE_KEY=key, LISTEN_PORT=str(PORT), SNAPSHOT_FILE=snapshot, REFRESH_SECONDS="600")
relay = subprocess.Popen([sys.executable, "-u", os.path.join(HERE, "eink_nas_relay.py")], env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def device_request():
    timestamp, nonce = str(int(time.time())), os.urandom(16).hex()
    signature = hmac.new(auth_key, "GET\n/\n{}\n{}".format(timestamp, nonce).encode(), hashlib.sha256).hexdigest()
    request = urllib.request.Request("http://127.0.0.1:{}/".format(PORT), headers={
        "x-device-time": timestamp, "x-device-nonce": nonce, "x-device-signature": signature})
    started = time.time()
    with urllib.request.urlopen(request, timeout=30) as response:
        body = json.loads(response.read())
        source_header = response.headers.get("x-eink-source")
    elapsed = time.time() - started
    node = r"""
const c = require('crypto'); const [k, b, t, n] = JSON.parse(require('fs').readFileSync(0, 'utf8'));
const s = Buffer.from(b.data, 'base64'); const d = c.createDecipheriv('aes-256-gcm', Buffer.from(k, 'hex'), Buffer.from(b.iv, 'base64'));
d.setAAD(Buffer.from(`v1\n${t}\n${n}`)); d.setAuthTag(s.subarray(-16));
const p = JSON.parse(Buffer.concat([d.update(s.subarray(0, -16)), d.final()]).toString());
process.stdout.write(JSON.stringify({ok: p.ok, date: p.date, events: (p.events||[]).length, todos: (p.todos||[]).length, notes: (p.notes||[]).length}));
"""
    assert body["n"] == nonce and body["t"] == int(timestamp)
    summary = json.loads(subprocess.run(["node", "-e", node], input=json.dumps([enc_key, body, timestamp, nonce]),
                                        capture_output=True, text=True, check=True).stdout)
    return source_header, elapsed, summary


try:
    deadline = time.time() + 60
    lines = []
    while time.time() < deadline:
        line = relay.stdout.readline()
        lines.append(line.rstrip())
        if "refreshed snapshot" in line or "refresh failed" in line:
            break
    print("relay log:", *lines, sep="\n  ")

    try:
        urllib.request.urlopen("http://127.0.0.1:{}/".format(PORT), timeout=5)
        raise AssertionError("unsigned request was accepted")
    except urllib.error.HTTPError as error:
        assert error.code == 401
    print("unsigned request -> 401 OK")

    for i in range(3):
        source_header, elapsed, summary = device_request()
        print("device request #{}: source={} {:.3f}s decrypted={}".format(i + 1, source_header, elapsed, summary))
        assert summary["ok"] is True and source_header in ("nas-cache", "nas-live")
    print("END-TO-END OK")
finally:
    relay.terminate()
    relay.wait(timeout=10)
    if os.path.exists(snapshot):
        os.remove(snapshot)
