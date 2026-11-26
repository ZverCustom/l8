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
if any series failed. All connections are registered for cleanup the moment they exist.
One absolute deadline (--budget) covers the HTTP preflight, USB reads, connects, every
wait, the result file and cleanup; a cut hold/observation is NOT_MEASURED, never PASS.
A subscription counts only with 200, a first valid event and the stream still open; a slow
subscriber that ends before the hold is a preparation failure. With --port, the board's
bootId is read over USB around every series: no valid answer = NOT_MEASURED (no reset
claimed), a changed bootId = reset. Without --port the result says usbCheck: disabled.
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
        self.closed_at = None
        self.mine_closed = False

    MAX_PASS = 128 * 1024  # bytes per poll: other clients and the deadline get control again

    def poll(self):
        if self.closed or self.mine_closed: return
        self.sock.setblocking(False)
        got = 0
        try:
            while got < self.MAX_PASS:
                chunk = self.sock.recv(65536)
                if not chunk: self.closed = True; break
                self.buf += chunk; got += len(chunk)
        except (BlockingIOError, InterruptedError): pass
        except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError): self.closed = self.reset = True
        except OSError: self.closed = True
        if self.closed and self.closed_at is None: self.closed_at = self.now()
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
        """fin: read what is there (one bounded pass), then FIN; unread: close with unread data;
        rst: SO_LINGER 0; quick: plain close without reading (cleanup)."""
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

    def budget(self, until=None):
        """Seconds left before the overall deadline and the optional series deadline."""
        left = self.dl.left()
        return left if until is None else max(0.0, min(left, until - self.now()))

    def open(self, kind, rcvbuf=None, legacy=False, until=None):
        self.n += 1
        name = f'{kind}{self.n}'
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if rcvbuf: s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        c = Sse(name, s, self.now); self.all.append(c)  # registered before anything can fail
        try:
            left = self.budget(until)
            if left <= 0: raise TimeoutError('no time left to connect')
            s.settimeout(min(self.connect_timeout, left))  # also bounds sendall
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
        if c.closed: return False, 'stream closed right after the first event (not an open subscription)'
        return True, 'ok'

    def alive(self, conns, seconds=2.0):
        """Each conn is open and delivers at least one new valid event."""
        start = {c.name: len(c.events) for c in conns}
        self.wait(lambda: all(len(c.events) > start[c.name] or c.closed for c in conns), seconds, conns)
        bad = [c.name for c in conns if c.closed or len(c.events) <= start[c.name] or any(e is None or e.get('bootId') != self.boot for e in c.events[start[c.name]:])]
        return not bad, bad

    def boot_now(self):
        """('disabled'|'ok'|'missing'|'error', value). Only 'ok' is a bootId."""
        if not self.usb_boot: return 'disabled', None
        try:
            import inspect
            takes_left = len(inspect.signature(self.usb_boot).parameters) > 0
            v = self.usb_boot(self.dl.left()) if takes_left else self.usb_boot()
        except Exception as e:
            return 'error', repr(e)
        return ('ok', v) if isinstance(v, str) and len(v) == 32 else ('missing', v)

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
        if before[0] != 'disabled':
            res['usb'] = {'before': list(before), 'after': list(after)}
            if before[0] != 'ok' or after[0] != 'ok':
                # No valid USB answer: a reset cannot be excluded, so no PASS (and no reset claim).
                res['usbProblem'] = 'USB bootId not read before and after the series'
                if res['outcome'] == 'PASS': res['outcome'] = 'NOT_MEASURED'
            elif before[1] != after[1] or after[1] != self.boot:
                res['reset'] = f'bootId changed during the series ({before[1]} -> {after[1]}, HTTP {self.boot})'
                if res['outcome'] == 'PASS': res['outcome'] = 'NOT_MEASURED'
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
        cut = end < t_close + self.observe  # the overall deadline shortened the observation
        while self.now() < end:
            p = self.open('probe', until=end)
            ok, why = self.admitted(p, min(2.0, max(0.05, end - self.now())))
            probes.append({'t': round(self.now() - t_close, 3), 'conn': p.name, 'result': p.status() or why, **({} if ok else {'reason': why})})
            if ok:
                accepted_at, newcomer = self.now() - t_close, p; break
            p.close('fin')
            self.wait(lambda: False, min(self.probe_every, max(0.0, end - self.now())), [])
        observed = self.now() - t_close
        others_ok, others_bad = self.alive(self.members)
        res = {'close': info, 'closedConn': victim.name, 'probes': probes, 'othersAlive': others_ok,
               'acceptedAfterS': None if accepted_at is None else round(accepted_at, 3),
               'acceptedWithinWindow': accepted_at is not None and accepted_at <= self.window,
               'window': self.window, 'observe': self.observe, 'observedS': round(observed, 3), 'observationCut': cut}
        if newcomer: self.members.append(newcomer)
        if not others_ok: return {'outcome': 'FAIL', 'reason': f'other members lost: {others_bad}', **res}
        if accepted_at is None and observed < self.window:
            return {'outcome': 'NOT_MEASURED', 'reason': f'deadline: only {observed:.2f} s of the {self.window} s window observed', **res}
        if accepted_at is None:
            return {'outcome': 'FAIL', 'reason': f'slot not accepted within {observed:.2f} s' + (' (observation cut by the deadline)' if cut else ''), **res}
        if accepted_at > self.window: return {'outcome': 'FAIL', 'reason': f'slot accepted after {accepted_at:.2f} s > {self.window} s', **res}
        return {'outcome': 'PASS', **res}

    def slow(self):
        ok, bad = self.alive(self.members)
        if not ok or len(self.members) != 3:
            return {'outcome': 'SKIP', 'reason': f'group of three not proven (members {len(self.members)}, dead {bad})'}
        victim = self.members.pop(0); victim.close('fin'); t_close = self.now()
        slow, end = None, t_close + min(self.observe, self.dl.left())
        while self.now() < end:
            p = self.open('slow', rcvbuf=512, until=end)
            ok, why = self.admitted(p, min(2.0, max(0.05, end - self.now())))
            if ok: slow = p; break
            if p.code() == 200:  # accepted by the server, then ended: preparation failed, no retry
                self.series['fast'] = {'outcome': 'SKIP', 'reason': 'slow series preparation failed'}
                return {'outcome': 'FAIL', 'reason': f'preparation: slow subscriber {why}', 'slowConn': p.name,
                        'slowEvents': len(p.events), 'slowReset': p.reset}
            p.close('fin'); self.wait(lambda: False, self.probe_every, [])
        if slow is None: return {'outcome': 'FAIL', 'reason': 'slow subscriber not accepted (slot not freed)'}
        # Preparation: the subscription must still be open before backpressure starts. A stream
        # that ends right after its first event proves nothing about dropping slow clients.
        # Checked without reading (reading would let the OS grow the receive window).
        self.wait(lambda: _ended_without_data(slow), min(1.0, self.dl.left()), [])
        if slow.closed or _ended_without_data(slow):
            self.series['fast'] = {'outcome': 'SKIP', 'reason': 'slow series preparation failed'}
            return {'outcome': 'FAIL', 'reason': 'preparation: slow subscriber closed before the hold (not a slow-client drop)',
                    'slowConn': slow.name, 'slowEvents': len(slow.events), 'slowReset': slow.reset}
        # Hold: read only the fast members; the slow one is never read (backpressure).
        fast = list(self.members); start = {f.name: len(f.events) for f in fast}
        t_hold = self.now(); hold_end = t_hold + min(self.hold, self.dl.left())
        hold_full = hold_end >= t_hold + self.hold
        while self.now() < hold_end:
            for f in fast: f.poll()
            self.sleep(0.1)
        gained = {f.name: len(f.events) - start[f.name] for f in fast}
        fast_ok = all(not f.closed and gained[f.name] >= 2 and all(e is not None and e.get('bootId') == self.boot for e in f.events[start[f.name]:]) for f in fast)
        self.series['fast'] = {'outcome': ('PASS' if fast_ok else 'FAIL') if hold_full or not fast_ok else 'NOT_MEASURED',
                               'gained': gained, 'closed': [f.name for f in fast if f.closed], 'holdS': round(self.now() - t_hold, 3)}
        # Drain: only now read the slow socket; EOF/RST after the queued data = closed by the server.
        t_drain = self.now(); drain_end = t_drain + min(self.drain, self.dl.left())
        self.wait(lambda: slow.closed, max(0.0, drain_end - self.now()), [slow])
        res = {'slowConn': slow.name, 'slowEvents': len(slow.events), 'slowReset': slow.reset,
               'closedSeenAfterHoldStartS': None if slow.closed_at is None else round(slow.closed_at - t_hold, 3),
               'holdS': round(t_drain - t_hold, 3), 'holdComplete': hold_full,
               'note': 'server closed the unread subscriber during hold/drain; the backpressure cause itself is not observable from outside'}
        slow.close('quick')
        if not hold_full: return {'outcome': 'NOT_MEASURED', 'reason': 'deadline: hold incomplete', **res}
        if not slow.closed:
            if self.now() < t_drain + self.drain - 0.05:
                return {'outcome': 'NOT_MEASURED', 'reason': 'deadline: drain incomplete', **res}
            return {'outcome': 'FAIL', 'reason': 'slow subscriber still open after hold and drain', **res}
        return {'outcome': 'PASS', **res}

    def reconnect(self):
        t = self.now(); end = t + min(self.observe, self.dl.left()); probes = 0
        while self.now() < end:
            p = self.open('reconnect', until=end); probes += 1
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
            for c in self.all:  # no reading here: cleanup must not depend on the server
                if not c.mine_closed: c.close('quick')
                else:
                    try: c.sock.close()
                    except OSError: pass
        outcomes = [v['outcome'] for v in self.series.values()]
        overall = 'FAIL' if 'FAIL' in outcomes else 'PASS' if all(o == 'PASS' for o in outcomes) else 'NOT_MEASURED'
        return {'overall': overall, 'series': self.series, 'timeline': self.timeline,
                'connectionsOpened': len(self.all), 'allClosed': all(c.mine_closed for c in self.all)}


def _ended_without_data(c):
    """True if the peer closed/reset and nothing is queued (peek, nothing consumed)."""
    try:
        c.sock.setblocking(False)
        if c.sock.recv(1, socket.MSG_PEEK) == b'':
            c.closed = True; c.closed_at = c.closed_at or c.now(); return True
        return False
    except (BlockingIOError, InterruptedError):
        return False
    except OSError:
        c.closed = c.reset = True; c.closed_at = c.closed_at or c.now(); return True


def http_json(host, port, path, deadline, max_bytes=256 * 1024):
    """GET path within the overall deadline (connect, send and every read), bounded in size."""
    left = deadline.left()
    if left <= 0: raise TimeoutError(f'{path}: no time left')
    s = socket.create_connection((host, port), timeout=min(5.0, left))
    try:
        s.settimeout(max(0.01, deadline.left()))
        s.sendall(f'GET {path} HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n'.encode())
        data = b''
        while True:
            left = deadline.left()
            if left <= 0: raise TimeoutError(f'{path}: deadline while reading')
            s.settimeout(left)
            chunk = s.recv(65536)
            if not chunk: break
            data += chunk
            if len(data) > max_bytes: raise ValueError(f'{path}: response larger than {max_bytes} bytes')
    finally:
        s.close()
    head, sep, body = data.partition(b'\r\n\r\n')
    if not sep or b' 200 ' not in head.split(b'\r\n', 1)[0] + b' ': raise ValueError(f'{path}: bad response {head[:40]!r}')
    return json.loads(body)


def main(argv=None, now=time.monotonic):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--base', default='http://192.168.4.1')
    ap.add_argument('--port', help='USB port for bootId checks (held elsewhere, so opening does not reset)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--budget', type=float, default=180)
    ap.add_argument('--window', type=float, default=3.0)
    ap.add_argument('--observe', type=float, default=15.0)
    a = ap.parse_args(argv)
    dl = Deadline(a.budget, now)  # one absolute deadline: preflight, series, result and cleanup
    u = urlsplit(a.base); host, port = u.hostname, u.port or 80
    res, serial_port = {'overall': 'NOT_MEASURED', 'series': {}, 'base': a.base, 'usbCheck': 'enabled' if a.port else 'disabled'}, None
    try:
        info = http_json(host, port, '/api/info', dl); token = http_json(host, port, '/api/session', dl)['token']
        usb_boot = None
        if a.port:
            import serial, uuid
            from e1_partial_request import read_status
            serial_port = serial.Serial(); serial_port.port = a.port; serial_port.baudrate = 115200; serial_port.timeout = .3
            serial_port.dtr = False; serial_port.rts = False; serial_port.open()
            usb_boot = lambda left: (read_status(serial_port, f'sse-{uuid.uuid4().hex[:6]}', timeout=max(0.0, min(6.0, left))) or {}).get('bootId')
        res.update(Diag(host, port, token, info['bootId'], dl, now=now, window=a.window, observe=a.observe, usb_boot=usb_boot).run())
        res.update(bootId=info['bootId'], firmware=info.get('firmware'))
    except Exception as e:
        res['error'] = f'{type(e).__name__}: {e}'
    finally:
        if serial_port is not None:
            try: serial_port.close()
            except Exception: pass
    res['elapsedS'] = round(a.budget - dl.left(), 3)
    with open(a.out, 'w') as f: json.dump(res, f, indent=1)
    print(json.dumps({'overall': res['overall'], **{k: v['outcome'] for k, v in res['series'].items()}, **({'error': res['error']} if 'error' in res else {})}))
    return 0 if res['overall'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
