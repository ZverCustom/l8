#!/usr/bin/env python3
"""Read-only HTTP/USB parity, auth and SSE v1 check for P1-C (no configuration change).
Waits for the board's STA link; retries only reads and validation-refused requests."""
import argparse, json, socket, time, urllib.request, urllib.error
from pathlib import Path
import serial
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--port', required=True); ap.add_argument('--log', type=Path); ap.add_argument('--wait', type=int, default=300)
args = ap.parse_args()
s = serial.Serial(); s.port = args.port; s.baudrate = 115200; s.timeout = .25; s.dtr = False; s.rts = False; s.open()
log, results = [], []
def usb(o):
    s.write((json.dumps(o, separators=(',', ':')) + '\n').encode()); s.flush(); u = time.monotonic() + 12
    while time.monotonic() < u:
        raw = s.readline()
        while raw and not raw.endswith(b'\n') and time.monotonic() < u: raw += s.readline()  # readline() returns early on a pause
        l = raw.decode(errors='replace').strip()
        if l: log.append(l)
        if l.startswith('L8 {'): return json.loads(l[3:])
    raise TimeoutError(str(o))
def check(n, v):
    assert v, n
    results.append(n); print('PASS', n, flush=True)
def err(r, code): return r.get('ok') is False and r.get('error', {}).get('code') == code
def retry(fn):
    for a in range(12):
        try: return fn()
        except urllib.error.HTTPError: raise
        except (urllib.error.URLError, TimeoutError, ConnectionError, socket.timeout, OSError) as e:
            print('  network retry', a + 1, type(e).__name__, flush=True); time.sleep(2)
    raise TimeoutError('HTTP unreachable after retries')
try:
    st = usb({'cmd': 'status'}); end = time.monotonic() + args.wait; last = 0
    while st['wifi']['phase'] != 'online' and time.monotonic() < end:
        if st['wifi']['phase'] == 'ap_only' and time.monotonic() - last > 60: usb({'cmd': 'action', 'action': 'reconnect'}); last = time.monotonic()
        time.sleep(2); st = usb({'cmd': 'status'})
    if st['wifi']['phase'] != 'online': print('NOT RUN: Wi-Fi never online'); raise SystemExit(2)
    base = f"http://{st['ip']}"; host = st['ip']
    boot = usb({'v': 1, 'id': 'i', 'cmd': 'info'})['bootId']
    token = retry(lambda: json.loads(urllib.request.urlopen(base + '/api/session', timeout=15).read()))['token']
    def post(obj, tok=token):
        def once():
            req = urllib.request.Request(base + '/api/command', data=json.dumps(obj).encode(), headers={'Content-Type': 'application/json', **({'X-L8-Token': tok} if tok else {})})
            try: r = urllib.request.urlopen(req, timeout=15); return r.status, json.loads(r.read())
            except urllib.error.HTTPError as h: return h.code, json.loads(h.read())
        return retry(once)
    for msg in ({'v': 1, 'id': 'i1', 'cmd': 'info'}, {'v': 1, 'id': 'g1', 'cmd': 'get_config'}, {'v': 1, 'id': 'g2', 'cmd': 'get_config', 'view': 'effective', 'client': 'probe'}):
        code, h = post(msg); check(f"HTTP == USB for {msg['cmd']} {msg.get('view', '')}".strip(), code == 200 and h == usb(msg))
    for msg, http in (({'v': 1, 'id': 'b1', 'cmd': 'action', 'bootId': boot, 'action': 'reset', 'channel': 9}, 400),
                      ({'v': 1, 'id': 'b2', 'cmd': 'action', 'bootId': 'ffffffffffffffffffffffffffffffff', 'action': 'master', 'on': True}, 409),
                      ({'v': 1, 'id': 'b3', 'cmd': 'nope'}, 404), ({'v': 3, 'id': 'b4', 'cmd': 'info'}, 400),
                      ({'v': 1, 'id': 'b5', 'cmd': 'save', 'bootId': boot, 'client': 'probe', 'revision': 0}, 409)):
        code, h = post(msg); check(f"HTTP/USB same error {h.get('error', {}).get('code')} -> {http}", code == http and h == usb(msg))
    code, h = post({'v': 1, 'id': 'a', 'cmd': 'status'}, tok=None); check('no token -> 403 AUTH_REQUIRED', code == 403 and err(h, 'AUTH_REQUIRED') and h['id'] == 'a')
    code, h = post({'cmd': 'status'}); check('/api/command without v -> 400 UNSUPPORTED_VERSION', code == 400 and err(h, 'UNSUPPORTED_VERSION'))
    def raw(body, extra=''):
        def once():
            c = socket.create_connection((host, 80), timeout=15)
            c.sendall(f'POST /api/command HTTP/1.1\r\nHost: {host}\r\nX-L8-Token: {token}\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\nConnection: close\r\n\r\n'.encode() + body)
            data = b''
            while True:
                chunk = c.recv(65536)
                if not chunk: break
                data += chunk
            c.close(); return data
        return retry(once)
    big = b'{"v":1,"id":"big","cmd":"status","pad":"' + b'x' * 24600 + b'"}'
    resp = raw(big); check('HTTP 24.6 KB body -> 413 FRAME_TOO_LARGE', b' 413 ' in resp.split(b'\r\n', 1)[0] and b'FRAME_TOO_LARGE' in resp)
    resp = raw(b'{"v":1,"id":"x","cmd":"info"}garbage'); check('HTTP trailing garbage -> 400 BAD_JSON', b' 400 ' in resp.split(b'\r\n', 1)[0] and b'BAD_JSON' in resp)
    def sse(q):
        def once():
            c = socket.create_connection((host, 80), timeout=15); c.sendall(f'GET /api/events?{q} HTTP/1.1\r\nHost: {host}\r\n\r\n'.encode()); return c
        return retry(once)
    def event(c, until):
        buf = b''
        while time.monotonic() < until:
            buf += c.recv(65536)
            if b'data: ' in buf and b'\n\n' in buf.split(b'data: ', 1)[1]: return json.loads(buf.split(b'data: ', 1)[1].split(b'\n\n', 1)[0]), buf.split(b'data: ', 1)[1].split(b'\n\n', 1)[1]
        raise TimeoutError('no SSE event')
    c = sse(f'v=1&token={token}'); e1, rest = event(c, time.monotonic() + 8)
    buf = rest; until = time.monotonic() + 8
    while time.monotonic() < until and not (b'data: ' in buf and b'\n\n' in buf.split(b'data: ', 1)[1]): buf += c.recv(65536)
    e2 = json.loads(buf.split(b'data: ', 1)[1].split(b'\n\n', 1)[0]); c.close()
    check('SSE v1 envelope coherent', e1['v'] == 1 and e1['bootId'] == boot and e1['revision'] == e1['status']['configRevision'] and e1['savedRevision'] == e1['status']['savedRevision'] and e1['status']['bootId'] == boot)
    check('SSE v1 seq increases', e2['seq'] > e1['seq'])
    c = sse(f'token={token}'); e, _ = event(c, time.monotonic() + 8); c.close(); check('legacy SSE has no v envelope', 'v' not in e and 'configRevision' in e)
    c = sse(f'v=2&token={token}'); head = c.recv(4096); c.close(); check('SSE v=2 -> 400', b' 400 ' in head.split(b'\r\n', 1)[0])
    time.sleep(3); conns = [sse(f'v=1&token={token}') for _ in range(3)]; time.sleep(1.5)
    c4 = sse(f'token={token}'); head = c4.recv(4096); c4.close()
    for c in conns: c.close()
    check('4th SSE client -> 503 (limit shared by v1/legacy)', b' 503 ' in head.split(b'\r\n', 1)[0])
    print(f'{len(results)} checks passed')
finally:
    if args.log: args.log.write_text('\n'.join(log) + '\n')
