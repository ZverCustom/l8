#!/usr/bin/env python3
"""USB backup/restore of the L8 configuration for unattended runs (no network needed).
backup:  get_config -> file (0600; password is redacted by firmware, keepPassword=true)
restore: usb_restore.restore() -> confirmed write and identical content, else exit 1."""
import argparse, json, os, sys, time
import serial
from usb_restore import restore

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('mode', choices=['backup', 'restore']); ap.add_argument('--port', required=True); ap.add_argument('--file', required=True)
a = ap.parse_args()
s = serial.Serial(); s.port = a.port; s.baudrate = 115200; s.timeout = .5; s.dtr = False; s.rts = False; s.open()
def cmd(o, timeout=12):
    s.write((json.dumps(o, separators=(',', ':')) + '\n').encode()); s.flush(); end = time.monotonic() + timeout; raw = b''
    while time.monotonic() < end:
        raw += s.readline()
        if raw.endswith(b'\n'):
            line = raw.decode(errors='replace').strip(); raw = b''
            if line.startswith('L8 {'): return json.loads(line[3:])
    raise TimeoutError(o.get('cmd'))
time.sleep(7)  # opening USB restarts this board; wait for the boot self-check
for _ in range(10):
    if cmd({'cmd': 'status'}).get('otaConfirmed'): break
    time.sleep(1)
if a.mode == 'backup':
    cfg = cmd({'cmd': 'get_config'})
    fd = os.open(a.file, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as f: json.dump(cfg, f)
    print('backup ok'); sys.exit(0)
ok, why = restore(cmd, json.load(open(a.file)))
print('restore', 'CONFIRMED' if ok else 'NOT confirmed: ' + why); sys.exit(0 if ok else 1)
