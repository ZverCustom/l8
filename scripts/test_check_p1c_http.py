"""Offline test of target selection in check_p1c_http.py (no board, no network)."""
import unittest
from check_p1c_http import resolve_target


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


if __name__ == '__main__':
    unittest.main()
