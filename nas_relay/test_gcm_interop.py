"""Cross-checks the pure-Python AES-256-GCM against Node's crypto module."""
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eink_nas_relay import gcm_decrypt, gcm_encrypt  # noqa: E402

NODE = r"""
const c = require('crypto');
const cases = JSON.parse(require('fs').readFileSync(0, 'utf8'));
const out = cases.map(k => {
  const key = Buffer.from(k.key, 'hex'), iv = Buffer.from(k.iv, 'hex'), aad = Buffer.from(k.aad, 'hex');
  const sealed = Buffer.from(k.py_sealed, 'hex');
  const d = c.createDecipheriv('aes-256-gcm', key, iv);
  d.setAAD(aad); d.setAuthTag(sealed.subarray(-16));
  const plain = Buffer.concat([d.update(sealed.subarray(0, -16)), d.final()]).toString('hex');
  const e = c.createCipheriv('aes-256-gcm', key, iv);
  e.setAAD(aad);
  const ct = Buffer.concat([e.update(Buffer.from(k.plain, 'hex')), e.final(), e.getAuthTag()]).toString('hex');
  return { node_plain: plain, node_sealed: ct };
});
process.stdout.write(JSON.stringify(out));
"""

cases = []
for length in [0, 1, 15, 16, 17, 31, 32, 33, 100, 2400]:
    for _ in range(3):
        key, iv = os.urandom(32), os.urandom(12)
        aad, plain = os.urandom(length % 50), os.urandom(length)
        cases.append({"key": key.hex(), "iv": iv.hex(), "aad": aad.hex(), "plain": plain.hex(),
                      "py_sealed": gcm_encrypt(key, iv, plain, aad).hex()})

result = json.loads(subprocess.run(["node", "-e", NODE], input=json.dumps(cases),
                                   capture_output=True, text=True, check=True).stdout)
for case, node in zip(cases, result):
    assert node["node_plain"] == case["plain"], "Node failed to decrypt Python output"
    assert node["node_sealed"] == case["py_sealed"], "ciphertext/tag differ from Node"
    back = gcm_decrypt(bytes.fromhex(case["key"]), bytes.fromhex(case["iv"]),
                       bytes.fromhex(node["node_sealed"]), bytes.fromhex(case["aad"]))
    assert back.hex() == case["plain"], "Python failed to decrypt Node output"

tampered = bytearray(bytes.fromhex(cases[-1]["py_sealed"]))
tampered[0] ^= 1
try:
    gcm_decrypt(bytes.fromhex(cases[-1]["key"]), bytes.fromhex(cases[-1]["iv"]),
                bytes(tampered), bytes.fromhex(cases[-1]["aad"]))
    raise AssertionError("tampered ciphertext was accepted")
except ValueError:
    pass

print("GCM interop OK: {} cases identical to Node, tamper rejected".format(len(cases)))
