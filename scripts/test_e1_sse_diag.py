"""Tests of e1_sse_diag.py against a local controllable TCP server (fake_sse_board.py).
Real sockets, short time constants; no board, no network change."""
import json, socket, time, unittest

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

    def early_closer(self, nth, same_read):
        test = self
        class EarlyClose(FakeSseBoard):
            def __init__(self, **kw):
                self.count = 0; super().__init__(**kw)
            def _stream(self, c, v1):
                self.count += 1
                if self.count != nth: return super()._stream(c, v1)
                ev = 'event: status\ndata: ' + json.dumps({'v': 1, 'bootId': self.boot, 'seq': 1, 'status': {}}) + '\n\n'
                self._send(c, b'HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n' + ev.encode())
                if not same_read: time.sleep(0.3)
                c.shutdown(socket.SHUT_WR)
                return self._free('closed-early')
        b = EarlyClose(); test.boards.append(b); return b

    def test_slow_subscriber_closed_before_the_hold_is_a_preparation_failure(self):
        for same_read in (True, False):
            with self.subTest(same_read=same_read):
                b = self.early_closer(7, same_read); res = run(b)  # 7th accepted stream = the slow subscriber
                o = outcomes(res)
                self.assertEqual(o['slow'], 'FAIL', res['series']['slow']); self.assertIn('preparation', res['series']['slow']['reason'])
                self.assertEqual(o['fast'], 'SKIP'); self.assertEqual(o['reconnect'], 'SKIP'); self.assertEqual(res['overall'], 'FAIL')
                self.assertIn('closed-early', b.closes)

    def test_member_closed_right_after_acceptance_fails_setup(self):
        b = self.early_closer(1, True); res = run(b)
        self.assertEqual(outcomes(res)['setup'], 'FAIL')
        self.assertTrue(any('closed' in c['reason'] for c in res['series']['setup']['clients'].values()))

    def test_usb_answers_missing_error_or_changed(self):
        cases = {
            'both missing': (lambda: None, 'usbProblem'),
            'one missing': (iter([None] + ['b' * 32] * 99).__next__, 'usbProblem'),
            'error': (lambda: (_ for _ in ()).throw(TimeoutError('no answer')), 'usbProblem'),
            'changed': (iter(['b' * 32, 'c' * 32] + ['c' * 32] * 99).__next__, 'reset'),
        }
        for name, (cb, key) in cases.items():
            with self.subTest(name):
                b = self.board(); res = run(b, usb_boot=cb)
                setup = res['series']['setup']
                self.assertEqual(setup['outcome'], 'NOT_MEASURED'); self.assertIn(key, setup)
                if key == 'usbProblem': self.assertNotIn('reset', setup)
                self.assertNotEqual(res['overall'], 'PASS')
        b = self.board(); res = run(b, usb_boot=lambda left: b.boot)
        self.assertEqual(res['overall'], 'PASS'); self.assertEqual(res['series']['setup']['usb']['before'], ['ok', b.boot])

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
        res = run(b, usb_boot=lambda left: next(boots))
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


class MainDeadline(unittest.TestCase):
    """R25: production main() honours one overall deadline, including preflight, USB and cleanup."""
    def setUp(self):
        import tempfile
        self.out = tempfile.mkdtemp() + '/r.json'

    def result(self):
        with open(self.out) as f: return json.load(f)

    def test_dripping_http_preflight_is_cut_at_the_budget(self):
        srv = socket.socket(); srv.bind(('127.0.0.1', 0)); srv.listen(1); port = srv.getsockname()[1]
        import threading
        def drip():
            c, _ = srv.accept()
            with c:
                c.recv(4096)
                for _ in range(100):
                    try: c.sendall(b'x')
                    except OSError: break
                    time.sleep(0.05)
        threading.Thread(target=drip, daemon=True).start()
        t = time.monotonic(); code = diag.main(['--base', f'http://127.0.0.1:{port}', '--budget', '0.3', '--out', self.out])
        self.assertLess(time.monotonic() - t, 0.8); self.assertEqual(code, 1)
        r = self.result(); self.assertEqual(r['overall'], 'NOT_MEASURED'); self.assertIn('TimeoutError', r['error'])
        srv.close()

    def test_silent_http_preflight_is_cut_at_the_budget(self):
        srv = socket.socket(); srv.bind(('127.0.0.1', 0)); srv.listen(1); port = srv.getsockname()[1]
        import threading
        def silent():
            c, _ = srv.accept(); c.recv(4096); time.sleep(3); c.close()
        threading.Thread(target=silent, daemon=True).start()
        t = time.monotonic(); diag.main(['--base', f'http://127.0.0.1:{port}', '--budget', '0.3', '--out', self.out])
        self.assertLess(time.monotonic() - t, 0.8); self.assertIn('Timeout', self.result()['error'])
        srv.close()

    def test_silent_usb_is_bounded_reported_and_closed(self):
        import sys, types
        closed = []
        class Silent:
            def __init__(self): self.port = self.baudrate = self.timeout = self.dtr = self.rts = None
            def open(self): pass
            def reset_input_buffer(self): pass
            def write(self, b): pass
            def flush(self): pass
            def readline(self): time.sleep(0.05); return b''
            def close(self): closed.append(True)
        sys.modules['serial'] = types.SimpleNamespace(Serial=Silent)
        b = FakeSseBoard()
        try:
            t = time.monotonic(); code = diag.main(['--base', f'http://127.0.0.1:{b.port}', '--port', 'X', '--budget', '4', '--out', self.out])
            elapsed = time.monotonic() - t
        finally:
            b.stop(); del sys.modules['serial']
        r = self.result()
        self.assertLess(elapsed, 4.8); self.assertEqual(code, 1); self.assertEqual(closed, [True])
        self.assertEqual(r['usbCheck'], 'enabled'); self.assertNotEqual(r['overall'], 'PASS')
        self.assertIn('usbProblem', r['series']['setup'])

    def test_deadline_during_the_series_is_honest_and_closes_everything(self):
        b = FakeSseBoard()
        try:
            t = time.monotonic(); code = diag.main(['--base', f'http://127.0.0.1:{b.port}', '--budget', '9', '--out', self.out])
            elapsed = time.monotonic() - t
        finally:
            b.stop()
        r = self.result()
        self.assertLess(elapsed, 9.8); self.assertEqual(code, 1); self.assertNotEqual(r['overall'], 'PASS')
        self.assertIn('NOT_MEASURED', [v['outcome'] for v in r['series'].values()])
        self.assertTrue(r['allClosed'])

    def test_one_poll_reads_a_bounded_amount_of_a_continuous_stream(self):
        srv = socket.socket(); srv.bind(('127.0.0.1', 0)); srv.listen(1)
        import threading
        stop = []
        def flood():
            c, _ = srv.accept()
            c.sendall(b'HTTP/1.1 200 OK\r\n\r\n')
            while not stop:
                try: c.sendall(b'x' * 65536)
                except OSError: break
        threading.Thread(target=flood, daemon=True).start()
        s = socket.create_connection(srv.getsockname()); conn = diag.Sse('f', s, time.monotonic)
        time.sleep(0.5)
        conn.poll()
        self.assertLessEqual(len(conn.buf) + len(conn.head or b''), 256 * 1024, 'one pass is bounded, independent of the constant')
        stop.append(1); s.close(); srv.close()


if __name__ == '__main__':
    unittest.main()
