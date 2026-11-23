#!/usr/bin/env python3
"""Minimal case for the task-watchdog reset seen in the E1 run (2026-11-23).

Sends an HTTP request line plus an unterminated header and holds the connection, so
WebServer::_parseRequest waits inside Stream::readStringUntil. Before the Stream yield
patch the board's loop (core 0) starved IDLE0 and the task watchdog reset it. The USB
status uptime tells whether the board restarted (hold the port open elsewhere so that
opening it here does not itself reset the board).
Exit 0 = no reset (uptime kept growing), 1 = reset observed, 2 = could not measure."""
import argparse, json, socket, time
import serial

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--port', required=True); ap.add_argument('--host', default='192.168.4.1'); ap.add_argument('--hold', type=float, default=12)
a = ap.parse_args()
s = serial.Serial(); s.port = a.port; s.baudrate = 115200; s.timeout = .5; s.dtr = False; s.rts = False; s.open()
def uptime():
    s.reset_input_buffer(); s.write(b'{"cmd":"status"}\n'); end = time.monotonic() + 10; raw = b''
    while time.monotonic() < end:
        raw += s.readline()
        if raw.endswith(b'\n'):
            line = raw.decode(errors='replace').strip(); raw = b''
            if line.startswith('L8 {'): return json.loads(line[3:])['uptimeMs']
    return None
before = uptime()
c = socket.create_connection((a.host, 80), timeout=10)
c.sendall(b'GET /api/status HTTP/1.1\r\nHost: x')  # header line never terminated
time.sleep(a.hold); c.close(); time.sleep(3)
after = uptime()
print(json.dumps({'uptimeBeforeMs': before, 'uptimeAfterMs': after, 'holdS': a.hold}))
if before is None or after is None: raise SystemExit(2)
raise SystemExit(0 if after > before else 1)
