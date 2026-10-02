import assert from "node:assert/strict";
import { createHmac, createDecipheriv, randomBytes } from "node:crypto";
const worker = (await import(new URL("./worker.js", import.meta.url))).default;

const KEY = randomBytes(32).toString("hex");
const today = new Intl.DateTimeFormat("en-CA", { timeZone: "Asia/Shanghai", year: "numeric", month: "2-digit", day: "2-digit" }).format(new Date());
const payload = (date = today) => ({ ok: true, date, events: [], todos: [], notes: [], counts: {} });
const sub = (label) => createHmac("sha256", Buffer.from(KEY, "hex")).update(label).digest();

function makeKv(initial) {
  const store = new Map(initial ? [["dashboard-snapshot-v1", JSON.stringify(initial)]] : []);
  return { store, get: async (k) => (store.has(k) ? JSON.parse(store.get(k)) : null), put: async (k, v) => store.set(k, v) };
}
let upstreamMode = "ok", upstreamCalls = 0;
globalThis.fetch = async (url, init) => {
  upstreamCalls++;
  assert.ok(String(url).includes("key=" + KEY));
  const mode = Array.isArray(upstreamMode) ? upstreamMode.shift() : upstreamMode;
  if (mode === "404") return new Response("nf", { status: 404 });
  if (mode === "hang") return new Promise((_, rej) => init.signal.addEventListener("abort", () => rej(Object.assign(new Error("a"), { name: "AbortError" }))));
  return new Response(JSON.stringify(payload()), { status: 200 });
};
function signed(badSig = false) {
  const ts = String(Math.floor(Date.now() / 1000)), nonce = randomBytes(16).toString("hex");
  let sig = createHmac("sha256", sub("eink-calendar-auth-v1")).update(`GET\n/\n${ts}\n${nonce}`).digest("hex");
  if (badSig) sig = "0".repeat(64);
  return { ts, nonce, req: new Request("http://relay.test/", { headers: { "x-device-time": ts, "x-device-nonce": nonce, "x-device-signature": sig } }) };
}
async function call(env, badSig) {
  const pending = []; const ctx = { waitUntil: (p) => pending.push(p) };
  const { ts, nonce, req } = signed(badSig);
  const res = await worker.fetch(req, env, ctx); await Promise.all(pending);
  const body = await res.json();
  if (res.status !== 200) return { status: res.status, body };
  assert.equal(body.n, nonce); assert.equal(body.t, Number(ts));
  const sealed = Buffer.from(body.data, "base64");
  const d = createDecipheriv("aes-256-gcm", sub("eink-calendar-encryption-v1"), Buffer.from(body.iv, "base64"));
  d.setAAD(Buffer.from(`v1\n${ts}\n${nonce}`)); d.setAuthTag(sealed.subarray(-16));
  const plain = JSON.parse(Buffer.concat([d.update(sealed.subarray(0, -16)), d.final()]).toString());
  return { status: 200, source: res.headers.get("x-eink-source"), plain };
}
const envWith = (kv) => ({ DEVICE_KEY: KEY, GOOGLE_API_URL: "https://script.test/exec", EINK_CACHE: kv });
const snap = (ageMin, date) => ({ fetchedAt: Date.now() - ageMin * 60000, payload: payload(date) });

let r;
upstreamCalls = 0; r = await call(envWith(makeKv(snap(5))));
assert.equal(r.source, "kv"); assert.equal(upstreamCalls, 0); assert.equal(r.plain.date, today);
console.log("1 fresh KV -> served from KV, no Google call, decrypts OK");

let kv = makeKv(); upstreamMode = "ok"; r = await call(envWith(kv));
assert.equal(r.source, "live"); assert.ok(kv.store.size === 1);
console.log("2 empty KV + Google OK -> live, snapshot written");

upstreamMode = "404"; r = await call(envWith(makeKv(snap(45))));
assert.equal(r.source, "stale");
console.log("3 45-min KV + Google 404 -> stale snapshot served");

upstreamMode = "hang"; let t0 = Date.now(); r = await call(envWith(makeKv(snap(45))));
assert.equal(r.source, "stale"); const took = (Date.now() - t0) / 1000; assert.ok(took < 14);
console.log(`4 45-min KV + Google hangs -> stale after ${took.toFixed(1)}s (under device 20s timeout)`);

upstreamMode = "404"; r = await call(envWith(makeKv(snap(150))));
assert.equal(r.status, 502); assert.equal(r.body.status, 404);
console.log("5 KV older than 2h + Google 404 -> 502 (device health warning still works)");

upstreamMode = "404"; r = await call(envWith(makeKv(snap(5, "2000-01-01"))));
assert.equal(r.status, 502);
upstreamMode = "ok"; upstreamCalls = 0; r = await call(envWith(makeKv(snap(5, "2000-01-01"))));
assert.equal(r.source, "live"); assert.equal(upstreamCalls, 1);
console.log("6 yesterday's KV never served; goes live instead");

upstreamMode = "ok"; r = await call({ DEVICE_KEY: KEY, GOOGLE_API_URL: "https://script.test/exec" });
assert.equal(r.source, "live");
console.log("7 no KV binding -> behaves like the old relay");

r = await call(envWith(makeKv(snap(5))), true); assert.equal(r.status, 401);
console.log("8 bad signature -> 401");

kv = makeKv(); upstreamMode = ["404", "ok"]; upstreamCalls = 0;
const pend = []; await worker.scheduled({}, envWith(kv), { waitUntil: (p) => pend.push(p) }); await Promise.all(pend);
assert.equal(upstreamCalls, 2); assert.equal(kv.store.size, 1);
console.log("9 cron: first attempt 404, retry OK -> snapshot written");
console.log("ALL PASSED");
