"""Offline test of target selection in check_p1c_http.py (no board, no network)."""
import unittest
import json
from check_p1c_http import resolve_target, slow_client_check


class FakeBoard:
    def __init__(self, phases):
        self.phases = list(phases); self.calls = []
    def __call__(self, cmd):
        self.calls.append(cmd)
        if cmd['cmd'] == 'status':
            phase = self.phases.pop(0) if len(self.phases) > 1 else self.phases[0]
            return {'wifi': {'phase': phase}, 'ip': '10.0.0.7'}
        return {'ok': True}


class Clock:
    def __init__(self): self.t = 0.0
    def now(self): return self.t
    def sleep(self, s): self.t += s


def reconnects(board): return [c for c in board.calls if c.get('action') == 'reconnect']


class ResolveTarget(unittest.TestCase):
    def test_ap_mode_with_ap_only_board_runs_http_without_usb_or_reconnect(self):
        board = FakeBoard(['ap_only'])
        self.assertEqual(resolve_target(board, use_ap=True), ('http://192.168.4.1', '192.168.4.1'))
        self.assertEqual(board.calls, [])

    def test_explicit_base_url_never_waits_or_reconnects(self):
        board = FakeBoard(['ap_only'])
        self.assertEqual(resolve_target(board, base_url='http://192.168.4.1:80/'), ('http://192.168.4.1:80', '192.168.4.1'))
        self.assertEqual(reconnects(board), [])

    def test_sta_mode_reconnects_at_most_once_a_minute_and_reports_failure(self):
        board, clock = FakeBoard(['ap_only']), Clock()
        self.assertIsNone(resolve_target(board, wait=150, now=clock.now, sleep=clock.sleep))
        self.assertEqual(len(reconnects(board)), 3)  # t=0, ~62, ~124

    def test_sta_mode_uses_board_ip_when_online(self):
        board, clock = FakeBoard(['associating', 'online']), Clock()
        self.assertEqual(resolve_target(board, now=clock.now, sleep=clock.sleep), ('http://10.0.0.7', '10.0.0.7'))
        self.assertEqual(reconnects(board), [])


EVENT = lambda boot='B': b'data: ' + json.dumps({'v': 1, 'bootId': boot, 'seq': 1}).encode() + b'\n\n'
HEAD200 = b'HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\nretry: 3000\n\n'


class FakeSock:
    """kind: fast | slow_alive | slow_dropped | refused | probe_ok | probe_busy"""
    def __init__(self, kind, clock, close_at=None, drop_at=None):
        self.kind, self.clock, self.close_at, self.drop_at = kind, clock, close_at, drop_at
        self.t0, self.sent, self.headed, self.done, self.closed_by_us = clock.t, 0, False, False, False
    def setblocking(self, flag): pass
    def close(self): self.closed_by_us = True
    def recv(self, n):
        t = self.clock.t
        if self.done: return b''
        if self.kind in ('refused', 'probe_busy'):
            self.done = True; return b'HTTP/1.1 503 Service Unavailable\r\n\r\n{}'
        if self.kind == 'probe_ok':
            if self.headed: raise BlockingIOError
            self.headed = True; return HEAD200
        if self.close_at is not None and t >= self.close_at: self.done = True; return b''
        if self.kind == 'slow_dropped' and t >= self.drop_at:
            self.done = True; return b'data: {"partial"'   # buffered tail, then EOF on the next read
        out = b''
        if not self.headed: self.headed = True; out += HEAD200
        due = 1 if self.kind != 'fast' else 1 + int((t - self.t0) / 0.5)
        if self.kind != 'fast': due = 1
        while self.sent < due: out += EVENT(); self.sent += 1
        if out: return out
        raise BlockingIOError


def run(socks, clock):
    queue = list(socks)
    return slow_client_check(lambda q, rcv: queue.pop(0), 'tok', 'B', now=clock.now, sleep=clock.sleep)


class SlowClient(unittest.TestCase):
    def test_reviewer_false_positive_now_fails(self):
        c = Clock()  # one fast client dies, the slow one stays alive, the probe still gets 200
        ok, why = run([FakeSock('slow_alive', c), FakeSock('fast', c, close_at=3), FakeSock('fast', c), FakeSock('probe_ok', c)], c)
        self.assertFalse(ok); self.assertIn('fast client lost', why)

    def test_slow_alive_until_deadline_fails(self):
        c = Clock()
        ok, why = run([FakeSock('slow_alive', c), FakeSock('fast', c), FakeSock('fast', c), FakeSock('probe_ok', c)], c)
        self.assertFalse(ok); self.assertIn('slow client still open', why)

    def test_subscription_not_accepted_fails(self):
        c = Clock()
        ok, why = run([FakeSock('refused', c), FakeSock('fast', c), FakeSock('fast', c)], c)
        self.assertFalse(ok); self.assertIn('not accepted', why)

    def test_slot_not_freed_fails(self):
        c = Clock()
        ok, why = run([FakeSock('slow_dropped', c, drop_at=5), FakeSock('fast', c), FakeSock('fast', c), FakeSock('probe_busy', c)], c)
        self.assertFalse(ok); self.assertIn('slot not freed', why)

    def test_slow_dropped_fast_alive_slot_free_passes(self):
        c = Clock(); socks = [FakeSock('slow_dropped', c, drop_at=5), FakeSock('fast', c), FakeSock('fast', c), FakeSock('probe_ok', c)]
        ok, why = run(socks, c)
        self.assertTrue(ok, why); self.assertTrue(all(x.closed_by_us for x in socks))


if __name__ == '__main__':
    unittest.main()
