#!/usr/bin/env python3
"""Minimal case for the task-watchdog reset seen in the E1 run (2026-11-23).

Sends an HTTP request line plus an unterminated header and holds the connection, so
WebServer::_parseRequest waits inside Stream::readStringUntil. Before the Stream yield
patch the loop (core 0) starved IDLE0 and the task watchdog reset the board.

Reset detection uses the board's bootId from v1 status (fresh per boot), matched to
this script's request ids; uptime is only extra diagnostics. Keep the USB port held
open elsewhere so that opening it here does not reset the board itself. After the hold
it also measures how long the board needs to answer HTTP again.
Exit 0 = same boot (no reset), 1 = reset (bootId changed), 2 = NOT MEASURED."""
import argparse, json, socket, time, uuid


def read_status(port, request_id, timeout=10, now=time.monotonic):
    """v1 status whose envelope id equals request_id, else None (stale/foreign lines ignored)."""
    port.reset_input_buffer()
    port.write((json.dumps({'v': 1, 'id': request_id, 'cmd': 'status'}) + '\n').encode()); port.flush()
    end = now(); end += timeout; raw = b''
    while now() < end:
        raw += port.readline()
        if not raw.endswith(b'\n'): continue
        line = raw.decode(errors='replace').strip(); raw = b''
        if not line.startswith('L8 {'): continue
        try: msg = json.loads(line[3:])
        except ValueError: continue
        if msg.get('v') == 1 and msg.get('id') == request_id and msg.get('ok') and msg.get('bootId'):
            return {'bootId': msg['bootId'], 'uptimeMs': msg.get('result', {}).get('uptimeMs')}
    return None


def verdict(before, after):
    if not before or not after: return 2, 'NOT MEASURED'
    return (0, 'no reset (same bootId)') if before['bootId'] == after['bootId'] else (1, 'RESET (bootId changed)')


def http_recovery(host, budget=20, now=time.monotonic, sleep=time.sleep, connect=socket.create_connection):
    """Seconds until GET /api/info answers again after the held request, or None."""
    start = now()
    while now() - start < budget:
        try:
            c = connect((host, 80), timeout=3)
            c.sendall(f'GET /api/info HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n'.encode())
            data = c.recv(64); c.close()
            if data.startswith(b'HTTP/1.1 200'): return round(now() - start, 2)
        except OSError:
            pass
        sleep(0.5)
    return None


def main():
    import serial
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True); ap.add_argument('--host', default='192.168.4.1'); ap.add_argument('--hold', type=float, default=12)
    a = ap.parse_args()
    s = serial.Serial(); s.port = a.port; s.baudrate = 115200; s.timeout = .5; s.dtr = False; s.rts = False; s.open()
    tag = uuid.uuid4().hex[:8]
    before = read_status(s, f'pr-{tag}-before')
    c = socket.create_connection((a.host, 80), timeout=10)
    c.sendall(b'GET /api/status HTTP/1.1\r\nHost: x')  # header line never terminated
    time.sleep(a.hold); c.close()
    recovery = http_recovery(a.host)
    after = read_status(s, f'pr-{tag}-after')
    code, text = verdict(before, after)
    print(json.dumps({'result': text, 'before': before, 'after': after, 'holdS': a.hold, 'httpRecoveryS': recovery}))
    raise SystemExit(code)


if __name__ == '__main__':
    main()
