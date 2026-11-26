#!/usr/bin/env python3
"""SSE slot diagnostics for the 503 seen in P1-015. Read-only towards the configuration.

Every series starts from a proven state and records exactly what happened:
  setup      three v1 subscriptions, each with HTTP 200 and a first valid event (bootId).
  admission  with the group proven, a 4th subscription must get 503.
  release    one member is closed in a given way, then probes measure when the slot is
             accepted again: `fin` (all data read, then FIN), `unread` (closed with data
             still unread, which makes the OS send RST), `rst` (SO_LINGER 0). Outcome
             PASS only if accepted within --window seconds; otherwise the real delay or
             "not freed within --observe". The accepted probe becomes the new member.
  slow       one member is replaced by a subscriber with a tiny receive window that never
             reads; the server must drop it while
  fast       the two remaining members keep receiving valid events;
  reconnect  after the drop a new subscription is accepted (with its delay).
Dependent series are SKIP when their precondition failed; the overall result stays FAIL
if any series failed. All connections are registered for cleanup the moment they exist,
and every wait is bounded by one overall deadline. With --port, the board's bootId is read
over USB around every series to tell a reset from a slot problem.
Result JSON: {overall, series: {name: {outcome, ...}}, timeline: [...]}."""
import argparse, json, socket, struct, time
from urllib.parse import urlsplit


class Deadline:
    def __init__(self, seconds, now=time.monotonic):
        self.now, self.end = now, now() + seconds
    def left(self): return max(0.0, self.end - self.now())
    def expired(self): return self.left() <= 0


class Sse:
    """One SSE connection: incremental header/event parsing (any fragmentation), EOF vs RST."""
    def __init__(self, name, sock, now):
        self.name, self.sock, self.now = name, sock, now
        self.buf, self.head, self.events, self.closed, self.reset, self.opened = b'', None, [], False, False, now()
        self.first_event_at = None
        self.mine_closed = False

    def poll(self):
        if self.closed or self.mine_closed: return
        self.sock.setblocking(False)
        try:
            while True:
                chunk = self.sock.recv(65536)
                if not chunk: self.closed = True; break
                self.buf += chunk
        except (BlockingIOError, InterruptedError): pass
        except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError): self.closed = self.reset = True
        except OSError: self.closed = True
        if self.head is None and b'\r\n\r\n' in self.buf:
            self.head, self.buf = self.buf.split(b'\r\n\r\n', 1)
        while self.head is not None and b'\n\n' in self.buf:
            block, self.buf = self.buf.split(b'\n\n', 1)
            data = [l[5:].lstrip() for l in block.split(b'\n') if l.startswith(b'data:')]
            if not data: continue
            try: ev = json.loads(b'\n'.join(data))
            except ValueError: ev = None
            self.events.append(ev)
            if self.first_event_at is None: self.first_event_at = self.now()

    def status(self):
        return self.head.split(b'\r\n', 1)[0].decode(errors='replace') if self.head else ''

    def code(self):
        parts = self.status().split(' ')
        return int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else None

    def unread_bytes(self):
        try:
            import fcntl, termios
            return struct.unpack('i', fcntl.ioctl(self.sock, termios.FIONREAD, b'\0\0\0\0'))[0]
        except (OSError, ImportError):
            return None

    def close(self, mode='fin'):
        """fin: read what is there, then FIN; unread: close with unread data; rst: SO_LINGER 0."""
        if self.mine_closed: return {'mode': mode, 'already': True}
        info = {'mode': mode}
        try:
            if mode == 'fin':
                self.poll(); info['unreadBytes'] = self.unread_bytes() or 0
                try: self.sock.shutdown(socket.SHUT_WR)
                except OSError: pass
            elif mode == 'unread':
                info['unreadBytes'] = self.unread_bytes()
            elif mode == 'rst':
                self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            self.sock.close()
        except OSError as e:
            info['error'] = repr(e)
        self.mine_closed = True
        return info


class Diag:
    def __init__(self, host, port, token, boot, deadline, now=time.monotonic, sleep=time.sleep,
                 window=3.0, observe=15.0, accept=8.0, probe_every=0.5, hold=12.0, drain=4.0, usb_boot=None, connect_timeout=3.0):
        self.host, self.port, self.token, self.boot, self.dl = host, port, token, boot, deadline
        self.now, self.sleep = now, sleep
        self.window, self.observe, self.accept, self.probe_every, self.hold, self.drain = window, observe, accept, probe_every, hold, drain
        self.usb_boot, self.connect_timeout = usb_boot, connect_timeout
        self.all, self.members, self.timeline, self.series = [], [], [], {}
        self.t0 = now(); self.n = 0

    # ---- primitives (all bounded by the deadline) ----
    def mark(self, what, **kw):
        self.timeline.append({'t': round(self.now() - self.t0, 3), 'event': what, **kw})

    def open(self, kind, rcvbuf=None, legacy=False):
        self.n += 1
        name = f'{kind}{self.n}'
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if rcvbuf: s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        c = Sse(name, s, self.now); self.all.append(c)  # registered before anything can fail
        try:
            s.settimeout(max(0.05, min(self.connect_timeout, self.dl.left())))
            s.connect((self.host, self.port))
            q = f'token={self.token}' if legacy else f'v=1&token={self.token}'
            s.sendall(f'GET /api/events?{q} HTTP/1.1\r\nHost: {self.host}\r\nAccept: text/event-stream\r\n\r\n'.encode())
        except OSError as e:
            c.closed = True; self.mark('open-failed', conn=name, error=repr(e))
        self.mark('open', conn=name, rcvbuf=rcvbuf)
        return c

    def wait(self, pred, seconds, conns):
        """Polls `conns` and, always, the group members: they are the fast clients that read
        continuously, so no wait of the diagnostic itself makes them slow."""
        end = self.now() + min(seconds, self.dl.left())
        while True:
            for c in {id(x): x for x in list(conns) + self.members}.values(): c.poll()
            if pred(): return True
            if self.now() >= end: return False
            self.sleep(0.05)

    def admitted(self, c, seconds):
        """(accepted, reason): 200 and a first valid event with the expected bootId."""
        self.wait(lambda: c.code() is not None and c.code() != 200 or bool(c.events) or c.closed, seconds, [c])
        if c.code() != 200: return False, f'status {c.status()!r}' if c.head else ('closed before headers' if c.closed else 'no headers in time')
        ev = c.events[0] if c.events else None
        if ev is None: return False, 'no valid first event'
        if ev.get('bootId') != self.boot: return False, f"bootId {ev.get('bootId')!r} != {self.boot!r}"
        return True, 'ok'

    def alive(self, conns, seconds=2.0):
        """Each conn is open and delivers at least one new valid event."""
        start = {c.name: len(c.events) for c in conns}
        self.wait(lambda: all(len(c.events) > start[c.name] or c.closed for c in conns), seconds, conns)
        bad = [c.name for c in conns if c.closed or len(c.events) <= start[c.name] or any(e is None or e.get('bootId') != self.boot for e in c.events[start[c.name]:])]
        return not bad, bad

    def boot_now(self):
        if not self.usb_boot: return None
        try: return self.usb_boot()
        except Exception as e: return f'error: {e!r}'

    def run_series(self, name, fn, needs=()):
        missing = [n for n in needs if self.series.get(n, {}).get('outcome') != 'PASS']
        if missing:
            self.series[name] = {'outcome': 'SKIP', 'reason': f'precondition not met: {missing}'}; return
        if self.dl.expired():
            self.series[name] = {'outcome': 'NOT_MEASURED', 'reason': 'session deadline reached'}; return
        before = self.boot_now()
        try:
            res = fn()
        except Exception as e:
            res = {'outcome': 'FAIL', 'reason': f'exception {e!r}'}
        after = self.boot_now()
        if before is not None or after is not None:
            res['usbBootBefore'], res['usbBootAfter'] = before, after
            if before != after or (after is not None and after != self.boot):
                res['outcome'] = 'NOT_MEASURED' if res['outcome'] == 'PASS' else res['outcome']
                res['reset'] = 'bootId changed during the series'
        self.series[name] = res; self.mark('series', name=name, outcome=res['outcome'])

    # ---- series ----
    def setup(self):
        group = [self.open('m') for _ in range(3)]
        # read the whole group while each one is checked, so none of them turns slow
        self.wait(lambda: all(c.events or c.closed or (c.code() not in (None, 200)) for c in group), self.accept, group)
        per = {}
        for c in group:
            ok, why = self.admitted(c, self.accept)
            per[c.name] = {'accepted': ok, 'reason': why, 'status': c.status(),
                           'firstEventMs': None if c.first_event_at is None else round((c.first_event_at - c.opened) * 1000)}
        if all(v['accepted'] for v in per.values()):
            self.members = group
            return {'outcome': 'PASS', 'clients': per}
        return {'outcome': 'FAIL', 'reason': 'group of three not created', 'clients': per}

    def admission(self):
        ok, bad = self.alive(self.members)
        if not ok: return {'outcome': 'SKIP', 'reason': f'group not proven alive before the 4th: {bad}'}
        c = self.open('fourth', legacy=True)
        self.wait(lambda: c.head is not None or c.closed, min(self.accept, 4.0), [c])
        code, status = c.code(), c.status()
        c.close('fin')
        ok2, bad2 = self.alive(self.members)
        res = {'fourthStatus': status, 'groupAliveAfter': ok2}
        if code == 503 and ok2: return {'outcome': 'PASS', **res}
        return {'outcome': 'FAIL', 'reason': 'expected 503 with the group alive' if code != 503 else f'group lost after the 4th: {bad2}', **res}

    def release(self, mode):
        ok, bad = self.alive(self.members)
        if not ok or len(self.members) != 3:
            return {'outcome': 'SKIP', 'reason': f'group of three not proven before closing (members {len(self.members)}, dead {bad})'}
        victim = self.members.pop(0)
        if mode == 'unread':  # stop reading the victim until data is queued in its socket
            end = self.now() + min(2.0, self.dl.left())
            while (victim.unread_bytes() or 0) == 0 and self.now() < end:
                for m in self.members: m.poll()
                self.sleep(0.05)
        t_close = self.now(); info = victim.close(mode); self.mark('close', conn=victim.name, **info)
        probes, accepted_at, newcomer = [], None, None
        end = t_close + min(self.observe, self.dl.left())
        while self.now() < end:
            p = self.open('probe')
            ok, why = self.admitted(p, min(2.0, max(0.05, end - self.now())))
            probes.append({'t': round(self.now() - t_close, 3), 'conn': p.name, 'result': p.status() or why})
            if ok:
                accepted_at, newcomer = self.now() - t_close, p; break
            p.close('fin')
            self.wait(lambda: False, min(self.probe_every, max(0.0, end - self.now())), [])
        others_ok, others_bad = self.alive(self.members)
        res = {'close': info, 'closedConn': victim.name, 'probes': probes, 'othersAlive': others_ok,
               'acceptedAfterS': None if accepted_at is None else round(accepted_at, 3),
               'acceptedWithinWindow': accepted_at is not None and accepted_at <= self.window,
               'window': self.window, 'observe': self.observe}
        if newcomer: self.members.append(newcomer)
        if not others_ok: return {'outcome': 'FAIL', 'reason': f'other members lost: {others_bad}', **res}
        if accepted_at is None: return {'outcome': 'FAIL', 'reason': f'slot not accepted within {self.observe} s', **res}
        if accepted_at > self.window: return {'outcome': 'FAIL', 'reason': f'slot accepted after {accepted_at:.2f} s > {self.window} s', **res}
        return {'outcome': 'PASS', **res}

    def slow(self):
        ok, bad = self.alive(self.members)
        if not ok or len(self.members) != 3:
            return {'outcome': 'SKIP', 'reason': f'group of three not proven (members {len(self.members)}, dead {bad})'}
        victim = self.members.pop(0); victim.close('fin'); t_close = self.now()
        slow, end = None, t_close + min(self.observe, self.dl.left())
        while self.now() < end:
            p = self.open('slow', rcvbuf=512)
            ok, why = self.admitted(p, min(2.0, max(0.05, end - self.now())))
            if ok: slow = p; break
            p.close('fin'); self.wait(lambda: False, self.probe_every, [])
        if slow is None: return {'outcome': 'FAIL', 'reason': 'slow subscriber not accepted (slot not freed)'}
        # Hold: read only the fast members; the slow one is never read (backpressure).
        fast = list(self.members); start = {f.name: len(f.events) for f in fast}
        hold_end = self.now() + min(self.hold, self.dl.left())
        while self.now() < hold_end:
            for f in fast: f.poll()
            self.sleep(0.1)
        gained = {f.name: len(f.events) - start[f.name] for f in fast}
        fast_ok = all(not f.closed and gained[f.name] >= 2 and all(e is not None and e.get('bootId') == self.boot for e in f.events[start[f.name]:]) for f in fast)
        self.series['fast'] = {'outcome': 'PASS' if fast_ok else 'FAIL', 'gained': gained, 'closed': [f.name for f in fast if f.closed]}
        # Drain: only now read the slow socket; EOF/RST after the queued data = dropped by the server.
        t_drain = self.now()
        self.wait(lambda: slow.closed, min(self.drain, self.dl.left()), [slow])
        res = {'slowConn': slow.name, 'droppedSeenAfterDrainS': round(self.now() - t_drain, 3) if slow.closed else None,
               'slowReset': slow.reset, 'slowEvents': len(slow.events)}
        slow.close('fin')
        if not slow.closed: return {'outcome': 'FAIL', 'reason': 'slow subscriber still open after hold and drain', **res}
        return {'outcome': 'PASS', **res}

    def reconnect(self):
        t = self.now(); end = t + min(self.observe, self.dl.left()); probes = 0
        while self.now() < end:
            p = self.open('reconnect'); probes += 1
            ok, why = self.admitted(p, min(2.0, max(0.05, end - self.now())))
            if ok:
                self.members.append(p); return {'outcome': 'PASS', 'acceptedAfterS': round(self.now() - t, 3), 'probes': probes}
            p.close('fin'); self.wait(lambda: False, self.probe_every, [])
        return {'outcome': 'FAIL', 'reason': f'no subscription accepted within {self.observe} s', 'probes': probes}

    def run(self, modes=('fin', 'unread', 'rst')):
        try:
            self.run_series('setup', self.setup)
            self.run_series('admission', self.admission, needs=('setup',))
            for m in modes:
                self.run_series(f'release_{m}', lambda m=m: self.release(m), needs=('setup',))
            self.run_series('slow', self.slow, needs=('setup',) + tuple(f'release_{m}' for m in modes[:1]))
            if 'fast' not in self.series: self.series['fast'] = {'outcome': 'SKIP', 'reason': 'slow series did not run'}
            self.run_series('reconnect', self.reconnect, needs=('slow',))
        finally:
            for c in self.all:
                if not c.mine_closed: c.close('fin')
                else:
                    try: c.sock.close()
                    except OSError: pass
        outcomes = [v['outcome'] for v in self.series.values()]
        overall = 'FAIL' if 'FAIL' in outcomes else 'PASS' if all(o == 'PASS' for o in outcomes) else 'NOT_MEASURED'
        return {'overall': overall, 'series': self.series, 'timeline': self.timeline,
                'connectionsOpened': len(self.all), 'allClosed': all(c.mine_closed for c in self.all)}


def http_json(host, port, path, timeout=5):
    s = socket.create_connection((host, port), timeout=timeout)
    try:
        s.sendall(f'GET {path} HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n'.encode())
        data = b''
        while True:
            chunk = s.recv(65536)
            if not chunk: break
            data += chunk
    finally:
        s.close()
    return json.loads(data.split(b'\r\n\r\n', 1)[1])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--base', default='http://192.168.4.1')
    ap.add_argument('--port', help='USB port for bootId checks (held elsewhere, so opening does not reset)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--budget', type=float, default=180)
    ap.add_argument('--window', type=float, default=3.0)
    ap.add_argument('--observe', type=float, default=15.0)
    a = ap.parse_args(argv)
    u = urlsplit(a.base); host, port = u.hostname, u.port or 80
    dl = Deadline(a.budget)
    info = http_json(host, port, '/api/info'); token = http_json(host, port, '/api/session')['token']
    usb_boot = None
    if a.port:
        import serial, uuid
        from e1_partial_request import read_status
        s = serial.Serial(); s.port = a.port; s.baudrate = 115200; s.timeout = .3; s.dtr = False; s.rts = False; s.open()
        usb_boot = lambda: (read_status(s, f'sse-{uuid.uuid4().hex[:6]}', timeout=6) or {}).get('bootId')
    res = Diag(host, port, token, info['bootId'], dl, window=a.window, observe=a.observe, usb_boot=usb_boot).run()
    res.update(base=a.base, bootId=info['bootId'], firmware=info.get('firmware'))
    with open(a.out, 'w') as f: json.dump(res, f, indent=1)
    print(json.dumps({'overall': res['overall'], **{k: v['outcome'] for k, v in res['series'].items()}}))
    return 0 if res['overall'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
