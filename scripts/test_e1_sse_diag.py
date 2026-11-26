"""Tests of e1_sse_diag.py against a local controllable TCP server (fake_sse_board.py).
Real sockets, short time constants; no board, no network change."""
import socket, time, unittest

import e1_sse_diag as diag
from fake_sse_board import FakeSseBoard

FAST = dict(window=0.6, observe=2.5, accept=1.5, probe_every=0.1, hold=4.0, drain=2.0)


def run(board, budget=40, usb_boot=None, modes=('fin', 'unread', 'rst'), **kw):
    dl = diag.Deadline(budget)
    d = diag.Diag('127.0.0.1', board.port, 't', board.boot, dl, usb_boot=usb_boot, **{**FAST, **kw})
    t = time.monotonic(); res = d.run(modes); res['elapsed'] = time.monotonic() - t
    return res


def outcomes(res): return {k: v['outcome'] for k, v in res['series'].items()}


class SseDiag(unittest.TestCase):
    def setUp(self): self.boards = []

    def board(self, **kw):
        b = FakeSseBoard(**kw); self.boards.append(b); return b

    def tearDown(self):
        for b in self.boards: b.stop()

    def assertCleaned(self, b, res):
        self.assertTrue(res['allClosed'])
        end = time.monotonic() + 3
        while b.occupied and time.monotonic() < end: time.sleep(0.05)
        self.assertEqual(b.occupied, 0, 'every connection of the diagnostic was closed and released')

    def test_healthy_board_passes_every_series(self):
        b = self.board(); res = run(b)
        self.assertEqual(res['overall'], 'PASS', outcomes(res))
        self.assertEqual(set(outcomes(res)), {'setup', 'admission', 'release_fin', 'release_unread', 'release_rst', 'slow', 'fast', 'reconnect'})
        for m in ('fin', 'unread', 'rst'):
            r = res['series'][f'release_{m}']
            self.assertTrue(r['acceptedWithinWindow']); self.assertLess(r['acceptedAfterS'], 0.6)
        self.assertEqual(res['series']['admission']['fourthStatus'].split(' ')[1], '503')
        self.assertCleaned(b, res)

    def test_close_modes_reach_the_server_as_fin_and_rst(self):
        b = self.board(); res = run(b, modes=('fin', 'unread', 'rst'))
        how = b.closes[:4]  # the 4th client (503) never occupies; members close in series order
        self.assertEqual(how[0], 'fin'); self.assertEqual(how[1], 'rst', 'closing with unread data resets'); self.assertEqual(how[2], 'rst')
        self.assertGreater(res['series']['release_unread']['close']['unreadBytes'], 0)
        self.assertEqual(res['series']['release_fin']['close']['unreadBytes'], 0)

    def test_slow_release_is_measured_not_passed(self):
        b = self.board(release_delay=1.2); res = run(b)
        r = res['series']['release_fin']
        self.assertEqual(r['outcome'], 'FAIL'); self.assertFalse(r['acceptedWithinWindow'])
        self.assertTrue(1.1 <= r['acceptedAfterS'] <= 2.3, r['acceptedAfterS'])
        self.assertTrue(any(p['result'].startswith('HTTP/1.1 503') for p in r['probes']), 'early probes were refused')
        self.assertEqual(res['overall'], 'FAIL')
        self.assertEqual(outcomes(res)['slow'], 'SKIP'); self.assertEqual(outcomes(res)['reconnect'], 'SKIP')
        self.assertCleaned(b, res)

    def test_leaked_slot_is_reported_and_dependents_skip(self):
        b = self.board(leak=1); res = run(b)
        r = res['series']['release_fin']
        self.assertEqual(r['outcome'], 'FAIL'); self.assertIsNone(r['acceptedAfterS']); self.assertIn('not accepted within', r['reason'])
        self.assertEqual(outcomes(res)['release_unread'], 'SKIP', 'only two members left: state not proven')
        self.assertEqual(res['overall'], 'FAIL')
        self.assertTrue(res['allClosed'])

    def test_write_error_only_release_still_frees_quickly_after_close(self):
        b = self.board(release='write-error'); res = run(b)
        self.assertEqual(outcomes(res)['release_fin'], 'PASS', res['series']['release_fin'])
        self.assertIn('write-error', b.closes)

    def test_slot_taken_by_someone_else_fails_setup_instead_of_passing_the_limit(self):
        b = self.board()
        stranger = socket.create_connection(('127.0.0.1', b.port))
        stranger.sendall(b'GET /api/events?v=1&token=t HTTP/1.1\r\nHost: x\r\n\r\n'); time.sleep(0.3)
        try:
            res = run(b)
        finally:
            stranger.close()
        s = res['series']['setup']
        self.assertEqual(s['outcome'], 'FAIL'); self.assertEqual(sum(not c['accepted'] for c in s['clients'].values()), 1)
        self.assertTrue(any('503' in c['reason'] for c in s['clients'].values()))
        self.assertTrue(all(v['outcome'] == 'SKIP' for k, v in res['series'].items() if k != 'setup'), outcomes(res))
        self.assertEqual(res['overall'], 'FAIL')

    def test_events_of_another_boot_are_not_an_admission(self):
        b = self.board()
        res = diag.Diag('127.0.0.1', b.port, 't', 'a' * 32, diag.Deadline(20), **FAST).run()
        s = res['series']['setup']
        self.assertEqual(s['outcome'], 'FAIL'); self.assertTrue(all('bootId' in c['reason'] for c in s['clients'].values()))

    def test_fourth_client_accepted_fails_admission(self):
        b = self.board(slots=4); res = run(b)
        self.assertEqual(outcomes(res)['setup'], 'PASS')
        a = res['series']['admission']
        self.assertEqual(a['outcome'], 'FAIL'); self.assertIn('200', a['fourthStatus'])
        self.assertEqual(res['overall'], 'FAIL')

    def test_too_few_slots_fails_setup(self):
        b = self.board(slots=2); res = run(b)
        self.assertEqual(outcomes(res)['setup'], 'FAIL'); self.assertEqual(outcomes(res)['admission'], 'SKIP')

    def test_fragmented_headers_events_and_delayed_accept(self):
        b = self.board(fragment=True, accept_delay=0.2); res = run(b, accept=2.0)
        self.assertEqual(res['overall'], 'PASS', outcomes(res))

    def test_server_that_never_drops_the_slow_subscriber(self):
        b = self.board(drop_slow=False); res = run(b)
        o = outcomes(res)
        self.assertEqual(o['slow'], 'FAIL'); self.assertEqual(o['fast'], 'PASS'); self.assertEqual(o['reconnect'], 'SKIP')

    def test_reset_seen_over_usb_marks_series_not_measured(self):
        b = self.board(); boots = iter([b.boot, b.boot, b.boot, 'c' * 32] + ['c' * 32] * 40)
        res = run(b, usb_boot=lambda: next(boots))
        self.assertEqual(res['series']['setup']['outcome'], 'PASS')
        self.assertEqual(res['series']['admission']['outcome'], 'NOT_MEASURED'); self.assertIn('reset', res['series']['admission'])
        self.assertNotEqual(res['overall'], 'PASS')

    def test_deadline_bounds_the_whole_run(self):
        b = self.board(release_delay=5); res = run(b, budget=3, observe=10)
        self.assertLess(res['elapsed'], 6)
        self.assertIn('NOT_MEASURED', outcomes(res).values())
        self.assertTrue(res['allClosed'])

    def test_board_unreachable(self):
        b = self.board(); b.stop(); time.sleep(0.1)
        res = run(b, budget=5)
        self.assertEqual(outcomes(res)['setup'], 'FAIL'); self.assertTrue(res['allClosed'])


if __name__ == '__main__':
    unittest.main()
