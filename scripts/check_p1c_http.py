#!/usr/bin/env python3
"""HTTP/USB parity, auth, 413 and SSE checks for P1-C. Configuration is never changed.

Target selection (resolve_target):
  --base-url URL   use this address; no STA wait and no reconnect
  --ap             same with http://192.168.4.1 (the computer must already be on ZVER-L8)
  default (STA)    wait for the board's STA link and, while it is in ap_only, send USB
                   action "reconnect" at most once a minute. This DOES change the board's
                   network state, so the default mode is not purely read-only.
HTTP requests are reads or requests refused by validation; on network errors they are
retried. USB is used for the parity reference and the bootId."""
import argparse, json, socket, time, urllib.request, urllib.error
from pathlib import Path


def resolve_target(usb, base_url=None, use_ap=False, wait=300, now=time.monotonic, sleep=time.sleep):
    """(base_url, host) or None. Explicit/AP targets never wait for STA and never reconnect."""
    if use_ap:
        base_url = 'http://192.168.4.1'
    if base_url:
        host = base_url.split('://', 1)[-1].split('/', 1)[0].split(':')[0]
        return base_url.rstrip('/'), host
    st = usb({'cmd': 'status'}); end = now() + wait; last = None
    while st['wifi']['phase'] != 'online' and now() < end:
        if st['wifi']['phase'] == 'ap_only' and (last is None or now() - last > 60):
            usb({'cmd': 'action', 'action': 'reconnect'}); last = now()
        sleep(2); st = usb({'cmd': 'status'})
    if st['wifi']['phase'] != 'online':
        return None
    return f"http://{st['ip']}", st['ip']


def main():
    import serial
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True); ap.add_argument('--log', type=Path); ap.add_argument('--wait', type=int, default=300)
    mode = ap.add_mutually_exclusive_group(); mode.add_argument('--base-url'); mode.add_argument('--ap', action='store_true')
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
        target = resolve_target(usb, args.base_url, args.ap, args.wait)
        if not target: print('NOT RUN: Wi-Fi never online'); raise SystemExit(2)
        base, host = target
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

        def raw(body):
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
        resp = raw(b'{"v":1,"id":"big","cmd":"status","pad":"' + b'x' * 24600 + b'"}'); check('HTTP 24.6 KB body -> 413 FRAME_TOO_LARGE', b' 413 ' in resp.split(b'\r\n', 1)[0] and b'FRAME_TOO_LARGE' in resp)
        resp = raw(b'{"v":1,"id":"x","cmd":"info"}garbage'); check('HTTP trailing garbage -> 400 BAD_JSON', b' 400 ' in resp.split(b'\r\n', 1)[0] and b'BAD_JSON' in resp)

        def sse(q, rcvbuf=None):
            def once():
                c = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                if rcvbuf: c.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)  # before connect: small advertised window
                c.settimeout(15); c.connect((host, 80)); c.sendall(f'GET /api/events?{q} HTTP/1.1\r\nHost: {host}\r\n\r\n'.encode()); return c
            return retry(once)

        def events(c, count, timeout=8):
            buf, out, until = b'', [], time.monotonic() + timeout
            while len(out) < count and time.monotonic() < until:
                buf += c.recv(65536)
                while b'data: ' in buf and b'\n\n' in buf.split(b'data: ', 1)[1]:
                    body, buf = buf.split(b'data: ', 1)[1].split(b'\n\n', 1); out.append(json.loads(body))
            if len(out) < count: raise TimeoutError('no SSE event')
            return out

        c = sse(f'v=1&token={token}'); e1, e2 = events(c, 2); c.close()
        check('SSE v1 envelope coherent', e1['v'] == 1 and e1['bootId'] == boot and e1['revision'] == e1['status']['configRevision'] and e1['savedRevision'] == e1['status']['savedRevision'] and e1['status']['bootId'] == boot)
        check('SSE v1 seq increases', e2['seq'] > e1['seq'])
        c = sse(f'token={token}'); e, = events(c, 1); c.close(); check('legacy SSE has no v envelope', 'v' not in e and 'configRevision' in e)
        c = sse(f'v=2&token={token}'); head = c.recv(4096); c.close(); check('SSE v=2 -> 400', b' 400 ' in head.split(b'\r\n', 1)[0])
        time.sleep(3); conns = [sse(f'v=1&token={token}') for _ in range(3)]; time.sleep(1.5)
        c4 = sse(f'token={token}'); head = c4.recv(4096); c4.close()
        for c in conns: c.close()
        check('4th SSE client -> 503 (limit shared by v1/legacy)', b' 503 ' in head.split(b'\r\n', 1)[0])
        # Slow client: a tiny receive window and no reads create real backpressure on the
        # board's socket; the server must drop it (pending > 2 s) and free its slot.
        time.sleep(3); slow = sse(f'v=1&token={token}', rcvbuf=512); fast = [sse(f'v=1&token={token}') for _ in range(2)]
        deadline = time.monotonic() + 20; freed = False
        while time.monotonic() < deadline and not freed:
            for f in fast: f.setblocking(False)
            for f in fast:
                try:
                    while f.recv(65536): pass
                except (BlockingIOError, socket.timeout): pass
            for f in fast: f.setblocking(True)
            probe = sse(f'token={token}'); head = probe.recv(4096); probe.close()
            freed = b' 200 ' in head.split(b'\r\n', 1)[0]
            if not freed: time.sleep(2)
        slow.setblocking(False); dropped = False
        try:
            while True:
                chunk = slow.recv(65536)
                if not chunk: dropped = True; break
        except BlockingIOError: pass
        except (ConnectionResetError, OSError): dropped = True
        for c in fast + [slow]: c.close()
        check('slow SSE client dropped under backpressure, slot freed', freed)
        print('  slow socket observed closed by server:', dropped, flush=True)
        c = sse(f'v=1&token={token}'); a, = events(c, 1); c.close(); check('SSE reconnect after drops works', a['bootId'] == boot)
        print(f'{len(results)} checks passed')
    finally:
        if args.log: args.log.write_text('\n'.join(log) + '\n')


if __name__ == '__main__':
    main()
