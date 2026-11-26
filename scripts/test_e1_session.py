"""Offline tests of e1_session.py: board, USB, flashing and network are simulated.
Nothing touches a real port, the Wi-Fi or the board."""
import json, os, stat, tempfile, unittest
from pathlib import Path

import e1_images
import e1_session as session
from test_e1_ap_run import FakeSystem as NetFake, AP

DEVICE = 'L8-001122AABBCC'
OLD_SHA, NEW_SHA = '09e092a' + 'a' * 33, '77804bc' + 'b' * 33


def make_image(root, role, commit, calls_delay):
    d = Path(root) / f'{role}-{commit[:7]}'; d.mkdir()
    files = {}
    for name in e1_images.FILES:
        (d / name).write_bytes(f'{role} {name}'.encode())
        files[name] = {'size': (d / name).stat().st_size, 'sha256': e1_images.sha256(d / name)}
    m = {'role': role, 'sourceCommit': commit, 'files': files, 'streamTimedReadCallsDelay': calls_delay,
         'flash': {'mode': 'dio', 'freq': '80m', 'size': '16MB', 'baud': 460800}}
    (d / 'manifest.json').write_text(json.dumps(m)); return d / 'manifest.json'


class Board(NetFake):
    """Network part from test_e1_ap_run plus a simulated board, USB and esptool."""
    def __init__(self, **f):
        super().__init__(**f)
        self.f.update({k: v for k, v in dict(port=True, hold_fail=False, device=DEVICE, image='old', master=False, output_on=False,
                                              esptool=True, flash_new=0, flash_old=0, old_reset=True, new_reset=False, recovery=1.8,
                                              partial='ok', usb_verify=0, config_changed_by_flash=False, settle=True).items()
                       if k not in f})
        self.image, self.boot, self.flashes = self.f['image'], 1, []
        self.open_handles, self.opened, self.closed_reads = set(), 0, 0
    def exists(self, path): return self.f['port']
    def now(self):
        self.now_calls = getattr(self, 'now_calls', 0) + 1
        if self.now_calls > 200000: raise AssertionError('loop guard: a wait never reached its deadline')
        return self.t
    # Port lifecycle as pyserial: every open is a new handle; release closes exactly that one.
    def hold_port(self, port):
        n = self.f['hold_fail']  # True: every open fails; integer N: open number N and later fail
        if n is True or (type(n) is int and n and self.opened + 1 >= n):
            raise OSError('no such port')
        super().hold_port(port); self.opened += 1; self.open_handles.add(self.opened); self.held = True; return self.opened
    def release_port(self, h):
        self.calls.append(['release']); self.open_handles.discard(h); self.held = bool(self.open_handles)
    def _check(self, h):
        if h not in self.open_handles: self.closed_reads += 1; raise OSError('Attempting to use a port that is not open')
    def usb_status(self, h):
        self._check(h)
        st = super().usb_status(h)
        if not self.f['settle']: st['otaConfirmed'] = False
        st.update(firmware='0.4.0+' + (OLD_SHA if self.image == 'old' else NEW_SHA)[:7], bootId=f'boot{self.boot}', master=self.f['master'],
                  channels=[{'on': self.f['output_on'] and i == 0, 'data': False} for i in range(8)])
        return st
    def usb_cmd(self, h, obj, timeout=8):
        self._check(h)
        return {'deviceId': self.f['device'], 'limits': {'channels': 8}} if obj.get('cmd') == 'info' else None
    def run(self, cmd, timeout=30):
        if len(cmd) > 2 and str(cmd[1]).endswith('esptool.py'):
            self.calls.append(cmd)
            if cmd[2] == 'version': return (0, 'esptool v4.5.1') if self.f['esptool'] else (1, 'missing')
            role = 'old' if any('old-' in str(c) for c in cmd) else 'new'
            r = self.f['flash_' + role]
            self.flashes.append(role)
            if r == 'hang': self.t += timeout; return 124, 'timeout'
            if r: return 2, 'A fatal error occurred'
            self.image = role; self.boot += 1; self.t += 30; return 0, 'Hash of data verified.'
        if len(cmd) > 2 and str(cmd[1]).endswith('e1_usb_config.py') and cmd[2] == 'verify':
            changed = self.f['config_changed_by_flash'] and self.image == 'new'
            return (1, 'verify DIFFERENT') if changed or self.f['usb_verify'] else (0, 'verify SAME')
        return super().run(cmd, timeout)
    def run_child(self, cmd, stdout_path, timeout, cwd):
        if any(str(c).endswith('e1_sse_diag.py') for c in cmd):
            self.calls.append(['child'] + cmd)
            role = 'old' if self.image == 'old' else 'new'
            behaviour = self.f.get('sse_' + role, 'PASS')
            if behaviour == 'timeout': self.t += timeout; return 'timeout'
            if behaviour == 'nofile': self.t += 5; return 1
            self.t += 60
            series = {k: 'PASS' for k in ('setup', 'admission', 'release_fin', 'release_unread', 'release_rst', 'slow', 'fast', 'reconnect')}
            if behaviour == 'FAIL': series['release_fin'] = 'FAIL'; series.update(slow='SKIP', fast='SKIP', reconnect='SKIP')
            out = cmd[cmd.index('--out') + 1]
            Path(out).write_text(json.dumps({'overall': behaviour, 'series': {k: {'outcome': v} for k, v in series.items()}}))
            return 0 if behaviour == 'PASS' else 1
        if any(str(c).endswith('e1_partial_request.py') for c in cmd):
            self.calls.append(['child'] + cmd)
            if self.f['partial'] == 'timeout': self.t += timeout; Path(stdout_path).write_text(''); return 'timeout'
            reset = self.f['old_reset'] if self.image == 'old' else self.f['new_reset']
            rec = None if reset else (self.f['recovery'] if self.image == 'new' else 0.5)
            if reset: self.boot += 1
            self.t += 35
            Path(stdout_path).write_text(json.dumps({'result': 'x', 'httpRecoveryS': rec}) + '\n')
            return 1 if reset else 0
        self.t += 60
        return super().run_child(cmd, stdout_path, timeout, cwd)


def run(board, extra=None, images=None):
    d = tempfile.mkdtemp()
    old, new = images or (make_image(d, 'old', OLD_SHA, False), make_image(d, 'new', NEW_SHA, True))
    argv = ['--port', 'P', '--device-id', DEVICE, '--old', str(old), '--new', str(new), '--return-ssid', 'HomeNet',
            '--return-confirmed', '--python', 'py', '--esptool', 'esptool.py']
    if extra is not None: argv = [a for a in argv if a not in extra.get('drop', [])] + extra.get('add', [])
    code = session.main(argv, sy=board, out_dir=d)
    rep = json.loads(next(Path(d).glob('*-e1-session.json')).read_text())
    return code, rep, d


def changes(b):
    net = [c for c in b.calls if c[0] == 'networksetup' and c[1] in ('-setairportnetwork', '-setairportpower', '-removepreferredwirelessnetwork')]
    return net, b.flashes


class Session(unittest.TestCase):
    def assertCleanedUp(self, b, rep):
        self.assertEqual(rep['configRestore'], 'CONFIRMED')
        self.assertTrue(rep['networkRestore']['confirmed']); self.assertEqual(b.network, 'home'); self.assertFalse(b.held)
        self.assertLessEqual(rep['sessionSeconds'], session.SESSION_LIMIT)

    def test_success_keeps_new_image(self):
        b = Board(); code, rep, _ = run(b)
        self.assertEqual(code, 0); self.assertEqual(rep['result'], 'PASS')
        self.assertTrue(rep['oldWdt'].startswith('reproduced')); self.assertEqual(rep['newImage'], 'no reset, HTTP recovered in 1.8 s')
        self.assertEqual((rep['usbHttpSse'], rep['browser']), ('PASS', 'PASS'))
        self.assertEqual(rep['imageLeft'], 'new (comparison and E1 passed)'); self.assertEqual(b.image, 'new'); self.assertEqual(b.flashes, ['new'])
        self.assertCleanedUp(b, rep)
        steps = [s['step'] for s in rep['steps']]
        order = ['preflight-static', 'preflight-board', 'usb-backup', 'join-ap', 'partial-old', 'flash', 'settled', 'config-after-flash',
                 'partial-new', 'check', 'browser', 'usb-config-restore', 'network-restore']
        first = [s for i, s in enumerate(steps) if s in order and s not in steps[:i]]
        self.assertEqual(first, order)
        for s in order: self.assertIn(s, steps)

    def test_preflight_refusals_change_nothing(self):
        cases = {
            'no user confirmation of the return network': (Board(), {'drop': ['--return-confirmed']}),
            'return network not preferred': (Board(), {'drop': ['--return-ssid', 'HomeNet'], 'add': ['--return-ssid', 'Elsewhere']}),
            'port missing': (Board(port=False), None),
            'port cannot be opened': (Board(hold_fail=True), None),
            'esptool missing': (Board(esptool=False), None),
            'pyserial missing': (Board(pyserial=False), None),
            'other board': (Board(device='L8-FFFFFFFFFFFF'), None),
            'board not on the old image': (Board(image='new'), None),
            'Master on': (Board(master=True), None),
            'an output on': (Board(output_on=True), None),
            'board does not settle': (Board(settle=False), None),
            'backup fails': (Board(usb_backup=1), None),
            'backup not verified': (Board(usb_verify=1), None),
        }
        for name, (b, extra) in cases.items():
            with self.subTest(name):
                code, rep, _ = run(b, extra)
                self.assertEqual(code, 2); self.assertIn('NOT RUN', rep['result']); self.assertEqual(changes(b), ([], []))

    def test_unverified_images_are_refused(self):
        d = tempfile.mkdtemp()
        tampered_old = make_image(d, 'old', OLD_SHA, False); (tampered_old.parent / 'firmware.bin').write_bytes(b'other')
        old_with_patch = make_image(tempfile.mkdtemp(), 'old', OLD_SHA, True)
        new_ok = make_image(tempfile.mkdtemp(), 'new', NEW_SHA, True)
        new_as_old = make_image(tempfile.mkdtemp(), 'new', NEW_SHA, True)
        for name, imgs in {'hash mismatch': (tampered_old, new_ok), 'old image contains the Stream patch': (old_with_patch, new_ok),
                           'roles swapped': (new_as_old, new_ok)}.items():
            with self.subTest(name):
                b = Board(); code, rep, _ = run(b, images=imgs)
                self.assertEqual(code, 2); self.assertEqual(changes(b), ([], []))

    def test_dry_run_changes_nothing(self):
        b = Board(); code, rep, _ = run(b, {'add': ['--dry-run']})
        self.assertEqual(code, 0); self.assertIn('dry-run', rep['result']); self.assertEqual(changes(b), ([], []))

    def assertRolledBack(self, b, rep, code):
        self.assertEqual(code, 3); self.assertEqual(rep['imageLeft'], 'old (rolled back)'); self.assertEqual(b.image, 'old')
        self.assertEqual(b.flashes[-1], 'old'); self.assertCleanedUp(b, rep)

    def test_new_flash_failure_rolls_back(self):
        b = Board(flash_new=1); code, rep, _ = run(b)
        self.assertIn('not measured', rep['newImage']); self.assertRolledBack(b, rep, code)

    def test_old_reset_not_reproduced_is_not_a_full_comparison(self):
        b = Board(old_reset=False); code, rep, _ = run(b)
        self.assertTrue(rep['oldWdt'].startswith('not reproduced')); self.assertEqual(rep['usbHttpSse'], 'PASS'); self.assertRolledBack(b, rep, code)

    def test_new_image_resets(self):
        b = Board(new_reset=True); code, rep, _ = run(b)
        self.assertEqual(rep['newImage'], 'RESET (bootId changed)'); self.assertRolledBack(b, rep, code)

    def test_no_http_recovery_is_not_pass(self):
        b = Board(recovery=None); code, rep, _ = run(b)
        self.assertEqual(rep['newImage'], 'no reset, but HTTP did not recover'); self.assertRolledBack(b, rep, code)

    def test_minimal_case_timeout_is_not_measured(self):
        b = Board(partial='timeout'); code, rep, _ = run(b)
        self.assertTrue(rep['oldWdt'].startswith('not measured')); self.assertTrue(rep['newImage'].startswith('not measured'))
        self.assertRolledBack(b, rep, code)

    def test_check_timeout_and_browser_failure(self):
        b = Board(child='timeout'); code, rep, _ = run(b)
        self.assertIn('timeout', rep['usbHttpSse']); self.assertRolledBack(b, rep, code)
        b = Board(browser=1); code, rep, _ = run(b)
        self.assertEqual(rep['usbHttpSse'], 'PASS'); self.assertEqual(rep['browser'], 'FAIL (1)'); self.assertRolledBack(b, rep, code)

    def test_config_changed_by_flash_stops_and_rolls_back(self):
        b = Board(config_changed_by_flash=True); code, rep, _ = run(b)
        self.assertIn('not verified', rep['newImage']); self.assertRolledBack(b, rep, code)

    def test_signal_during_session_runs_cleanup(self):
        b = Board(); real = b.run_child
        def child(cmd, *a):
            if any(str(c).endswith('check_p1c_http.py') for c in cmd): raise KeyboardInterrupt('signal 15')
            return real(cmd, *a)
        b.run_child = child
        code, rep, _ = run(b)
        self.assertIn('signal 15', rep['interrupted']); self.assertEqual(rep['usbHttpSse'], 'NOT RUN'); self.assertRolledBack(b, rep, code)

    def test_cleanup_failures_are_not_hidden(self):
        b = Board(usb_restore=1); code, rep, _ = run(b)
        self.assertEqual(code, 4); self.assertIn('NOT confirmed', rep['configRestore'])
        b = Board(rejoin='fail', autojoin=False); code, rep, _ = run(b)
        self.assertEqual(code, 4); self.assertFalse(rep['networkRestore']['confirmed'])
        b = Board(new_reset=True, flash_old=1); code, rep, _ = run(b)
        self.assertEqual(code, 5); self.assertIn('NOT confirmed', rep['imageLeft']); self.assertEqual(rep['configRestore'], 'CONFIRMED')
        self.assertTrue(rep['networkRestore']['confirmed'])

    def test_everything_hangs_stays_within_25_minutes(self):
        b = Board(partial='timeout', child='timeout', browser='timeout', flash_new='hang', rejoin='hang')
        code, rep, _ = run(b)
        self.assertNotEqual(code, 0); self.assertLessEqual(rep['sessionSeconds'], session.SESSION_LIMIT)
        self.assertEqual(rep['configRestore'], 'CONFIRMED')

    def test_slow_steps_skip_e1_instead_of_overrunning(self):
        b = Board(); real = b.run_child
        def slow(cmd, path, timeout, cwd):
            r = real(cmd, path, timeout, cwd)
            if any(str(c).endswith('e1_partial_request.py') for c in cmd): b.t += 300
            return r
        b.run_child = slow
        code, rep, _ = run(b)
        self.assertIn('no time left', rep['usbHttpSse']); self.assertEqual(code, 3)
        self.assertLessEqual(rep['sessionSeconds'], session.SESSION_LIMIT); self.assertRolledBack(b, rep, code)

    def test_step_that_does_not_fit_is_not_started(self):
        b = Board(); real_run, real_child = b.run, b.run_child
        def run_slow(cmd, timeout=30):
            if len(cmd) > 2 and str(cmd[1]).endswith('e1_usb_config.py') and cmd[2] in ('backup', 'verify'): b.t += 150
            return real_run(cmd, timeout)
        def child_slow(cmd, path, timeout, cwd):
            r = real_child(cmd, path, timeout, cwd)
            if any(str(c).endswith('e1_partial_request.py') for c in cmd): b.t += 400
            return r
        b.run, b.run_child = run_slow, child_slow
        code, rep, _ = run(b)
        self.assertIn({'steps': ['flash', 'settle']}, [{k: v for k, v in s.items() if k == 'steps'} for s in rep['steps'] if s['step'] == 'skip-no-time'])
        self.assertEqual(b.flashes, []); self.assertEqual(rep['imageLeft'], 'old (unchanged)'); self.assertEqual(b.image, 'old')
        self.assertEqual(code, 3); self.assertCleanedUp(b, rep)

    def test_late_cleanup_puts_the_network_first(self):
        """Cleanup starting late (a child overran its timeout): the rollback gets only its own
        sub-budget, so the network restore still has its reserve. The image is reported UNKNOWN."""
        b = Board(old_reset=True); real_run, real_child = b.run, b.run_child
        def child(cmd, path, timeout, cwd):
            res = real_child(cmd, path, timeout, cwd)
            if any(str(c).endswith('check_p1c_http.py') for c in cmd): b.t = max(b.t, session.SESSION_LIMIT - 350)
            return res
        def run_(cmd, timeout=30):
            t0 = b.t; res = real_run(cmd, timeout)
            if len(cmd) > 2 and str(cmd[1]).endswith('esptool.py') and cmd[2] != 'version': b.t = t0 + timeout
            if cmd[:2] == ['networksetup', '-setairportnetwork'] and cmd[3] == 'HomeNet': b.t += 100
            return res
        real_status = b.usb_status
        def status(h):
            st = real_status(h); b.t += 8
            if len(b.flashes) > 1: st['otaConfirmed'] = False  # the rolled-back board never settles
            return st
        b.run_child, b.run, b.usb_status = child, run_, status
        code, rep, _ = run(b)
        self.assertTrue(rep['networkRestore']['confirmed'], rep['steps'][-8:]); self.assertEqual(b.network, 'home')
        self.assertIn('UNKNOWN', rep['imageLeft']); self.assertEqual(code, 5)
        self.assertLessEqual(rep['sessionSeconds'], session.SESSION_LIMIT)

    # ---- R16: the port handle after reopening ----
    def test_reopened_port_is_used_after_flash_and_rollback(self):
        b = Board(); code, rep, _ = run(b)
        self.assertEqual(code, 0); self.assertEqual(b.closed_reads, 0); self.assertEqual(b.opened, 2)
        b = Board(new_reset=True); code, rep, _ = run(b)
        self.assertEqual(rep['imageLeft'], 'old (rolled back)'); self.assertEqual(b.closed_reads, 0); self.assertEqual(code, 3)

    def test_reopen_failure_after_flash_is_reported_and_cleanup_continues(self):
        b = Board(hold_fail=2); code, rep, _ = run(b)
        self.assertEqual(b.closed_reads, 0)
        self.assertIn('not measured', rep['newImage']); self.assertIn('UNKNOWN', rep['imageLeft'])
        self.assertTrue(rep['networkRestore']['confirmed']); self.assertEqual(b.network, 'home'); self.assertEqual(code, 5)

    # ---- R17: a join that may have changed the network is always followed by a restore ----
    def interrupted_join(self, when, nth=1, exc=KeyboardInterrupt):
        b = Board(); real = b.run; seen = []
        def run_(cmd, timeout=30):
            is_join = cmd[:2] == ['networksetup', '-setairportnetwork'] and cmd[3] == AP
            if is_join: seen.append(1)
            if is_join and len(seen) == nth and when == 'before': raise exc('signal before join')
            reply = real(cmd, timeout)
            if is_join and len(seen) == nth and when == 'after-effect': raise exc('signal after join took effect')
            if cmd[0] == 'ipconfig' and len(seen) == nth and when == 'address-wait' and b.network == 'ap': raise exc('signal in address wait')
            return reply
        b.run = run_
        return b

    def test_interrupted_join_always_restores_network(self):
        for when in ('before', 'after-effect', 'address-wait'):
            for nth in (1, 2):
                for exc in (KeyboardInterrupt, RuntimeError):
                    with self.subTest(when=when, join=nth, exc=exc.__name__):
                        b = self.interrupted_join(when, nth, exc); code, rep, _ = run(b)
                        self.assertEqual(b.network, 'home'); self.assertTrue(rep['networkRestore']['confirmed'])
                        self.assertEqual(rep['configRestore'], 'CONFIRMED'); self.assertEqual(code, 3)
                        if nth == 2: self.assertEqual(b.image, 'old', 'new image not kept after an aborted session')

    def test_interrupted_join_with_failed_restore_is_not_hidden(self):
        b = self.interrupted_join('after-effect'); b.f.update(rejoin='fail', autojoin=False)
        code, rep, _ = run(b)
        self.assertFalse(rep['networkRestore']['confirmed']); self.assertEqual(code, 5)

    # ---- R18: outputs must be proven off ----
    def test_unproven_outputs_refuse_before_any_change(self):
        def bad(mutate):
            b = Board(); real = b.usb_status
            def st(h):
                x = real(h); mutate(x); return x
            b.usb_status = st; return b
        cases = {
            'channels missing': lambda x: x.pop('channels'),
            'channels empty': lambda x: x.update(channels=[]),
            'channels short': lambda x: x.update(channels=x['channels'][:7]),
            'channels not a list': lambda x: x.update(channels={'0': {}}),
            'channel not an object': lambda x: x['channels'].__setitem__(3, 'off'),
            'on missing': lambda x: x['channels'][2].pop('on'),
            'data missing': lambda x: x['channels'][5].pop('data'),
            'on null': lambda x: x['channels'][0].update(on=None),
            'data as 0': lambda x: x['channels'][1].update(data=0),
            'master missing': lambda x: x.pop('master'),
            'master null': lambda x: x.update(master=None),
        }
        for name, mutate in cases.items():
            with self.subTest(name):
                b = bad(mutate); code, rep, _ = run(b)
                self.assertEqual(code, 2); self.assertIn('NOT RUN', rep['result']); self.assertEqual(changes(b), ([], []))
        b = Board(); real_cmd = b.usb_cmd
        b.usb_cmd = lambda h, obj, timeout=8: {**real_cmd(h, obj, timeout), 'limits': {'channels': 6}}
        code, rep, _ = run(b)
        self.assertEqual(code, 2); self.assertEqual(changes(b), ([], []))

    # ---- R19: the whole session stays within SESSION_LIMIT at worst admissible durations ----
    def test_worst_case_durations_stay_within_session_limit(self):
        class Timed(Board):
            """Review P1-013 model: every command and child takes its full timeout, every USB
            status/info read 8 s; the new image settles late; rollback never settles."""
            def __init__(self, delay, **f):
                super().__init__(**f); self.delay, self.open_at = delay, 0
            def hold_port(self, port):
                h = super().hold_port(port); self.open_at = self.t; return h
            def usb_status(self, h):
                self.t += 8; st = super().usb_status(h)
                if self.image == 'new' and self.t - self.open_at < self.delay: st['otaConfirmed'] = False
                if len(self.flashes) > 1 and self.image == 'old': st['otaConfirmed'] = False
                return st
            def usb_cmd(self, h, obj, timeout=8):
                self.t += timeout; return super().usb_cmd(h, obj, timeout)
            def run(self, cmd, timeout=30):
                t0 = self.t; res = super().run(cmd, timeout); self.t += max(0, timeout - (self.t - t0)); return res
            def run_child(self, cmd, path, timeout, cwd):
                t0 = self.t; res = super().run_child(cmd, path, timeout, cwd); self.t += max(0, timeout - (self.t - t0)); return res
        worst = 0
        for delay in (0, 30, 60, 90, 100, 110, 120, 125, 140, 200):
            for extra in ({'rejoin': 'hang', 'autojoin': False}, {'rejoin': 'hang'}, {}):
                with self.subTest(delay=delay, **extra):
                    b = Timed(delay, **extra); code, rep, _ = run(b)
                    worst = max(worst, rep['sessionSeconds'])
                    self.assertLessEqual(rep['sessionSeconds'], session.SESSION_LIMIT, rep['steps'][-6:])
                    if rep['imageLeft'].startswith('UNKNOWN') or not rep['networkRestore'].get('confirmed', True):
                        self.assertIn(code, (4, 5))
        print('worst session seconds', worst)

    def test_report_private_and_without_secrets(self):
        old = os.umask(0o022)
        try:
            b = Board(); code, rep, d = run(b)
            pwd = session.ap_run.ap_credentials()[1]
            for p in Path(d).iterdir():
                if p.is_file():
                    self.assertEqual(stat.S_IMODE(p.stat().st_mode) & 0o077, 0, p.name)
            text = next(Path(d).glob('*-e1-session.json')).read_text()
            self.assertNotIn(pwd, text)
        finally:
            os.umask(old)


class SseRoute(unittest.TestCase):
    def assertRestoredOld(self, b, rep):
        self.assertEqual(rep['imageLeft'], 'old (rolled back)'); self.assertEqual(b.image, 'old'); self.assertEqual(b.flashes, ['new', 'old'])
        self.assertEqual(rep['configRestore'], 'CONFIRMED'); self.assertTrue(rep['networkRestore']['confirmed']); self.assertFalse(b.held)
        self.assertLessEqual(rep['sessionSeconds'], session.SESSION_LIMIT)

    def children(self, b, name): return [c for c in b.calls if c[0] == 'child' and any(str(x).endswith(name) for x in c)]

    def test_both_diagnostics_pass_and_old_image_is_restored(self):
        b = Board(); code, rep, _ = run(b, {'add': ['--route', 'sse']})
        self.assertEqual(code, 0)
        self.assertEqual(rep['sseOld']['overall'], 'PASS'); self.assertEqual(rep['sseNew']['overall'], 'PASS')
        self.assertEqual(len(self.children(b, 'e1_sse_diag.py')), 2)
        for name in ('e1_partial_request.py', 'check_p1c_http.py', 'e1_browser.mjs'): self.assertEqual(self.children(b, name), [], name)
        self.assertRestoredOld(b, rep)
        steps = [x['step'] for x in rep['steps']]
        self.assertLess(steps.index('sse-old'), steps.index('flash')); self.assertLess(steps.index('settled'), steps.index('sse-new'))

    def test_failing_new_diagnostic_is_reported_per_series(self):
        b = Board(sse_new='FAIL'); code, rep, _ = run(b, {'add': ['--route', 'sse']})
        self.assertEqual(code, 3); self.assertEqual(rep['sseNew']['release_fin'], 'FAIL'); self.assertEqual(rep['sseNew']['slow'], 'SKIP')
        self.assertEqual(rep['sseOld']['overall'], 'PASS'); self.assertRestoredOld(b, rep)

    def test_diagnostic_without_result_or_timeout_is_not_measured(self):
        for how in ('nofile', 'timeout'):
            with self.subTest(how):
                b = Board(sse_old=how); code, rep, _ = run(b, {'add': ['--route', 'sse']})
                self.assertIn('not measured', rep['sseOld']); self.assertEqual(code, 3); self.assertRestoredOld(b, rep)

    def test_config_changed_by_flash_stops_before_the_new_diagnostic(self):
        b = Board(config_changed_by_flash=True); code, rep, _ = run(b, {'add': ['--route', 'sse']})
        self.assertIn('not measured', rep['sseNew']); self.assertEqual(len(self.children(b, 'e1_sse_diag.py')), 1)
        self.assertEqual(code, 3); self.assertRestoredOld(b, rep)

    def test_child_exit_and_result_completeness_decide_the_verdict(self):
        full = {k: {'outcome': 'PASS'} for k in session.SSE_SERIES}
        cases = {
            'PASS file, then timeout': ({'overall': 'PASS', 'series': full}, 'timeout'),
            'PASS file, exit 1': ({'overall': 'PASS', 'series': full}, 1),
            'empty series': ({'overall': 'PASS', 'series': {}}, 0),
            'one series missing': ({'overall': 'PASS', 'series': {k: v for k, v in full.items() if k != 'reconnect'}}, 0),
            'overall contradicts series': ({'overall': 'PASS', 'series': {**full, 'slow': {'outcome': 'FAIL'}}}, 0),
            'wrong types': ({'overall': 'PASS', 'series': {**full, 'fast': 'PASS'}}, 0),
            'unknown outcome': ({'overall': 'PASS', 'series': {**full, 'fast': {'outcome': 'OK'}}}, 0),
            'not an object': (['PASS'], 0),
        }
        for name, (payload, rc) in cases.items():
            with self.subTest(name):
                b = Board(); real = b.run_child
                def child(cmd, path, timeout, cwd, payload=payload, rc=rc):
                    if any(str(c).endswith('e1_sse_diag.py') for c in cmd):
                        real(cmd, path, timeout, cwd)
                        Path(cmd[cmd.index('--out') + 1]).write_text(json.dumps(payload)); return rc
                    return real(cmd, path, timeout, cwd)
                b.run_child = child
                code, rep, _ = run(b, {'add': ['--route', 'sse']})
                self.assertEqual(code, 3); self.assertNotEqual(rep['result'], 'PASS')
                self.assertEqual(rep['sseOld']['overall'], 'INVALID'); self.assertTrue(rep['sseOld']['problems'])
                self.assertRestoredOld(b, rep)

    def test_incomplete_result_is_invalid_even_if_it_claims_not_measured(self):
        partial = {k: {'outcome': 'PASS'} for k in session.SSE_SERIES[:3]}
        v = session.sse_verdict(1, {'overall': 'NOT_MEASURED', 'series': partial})
        self.assertEqual(v['overall'], 'INVALID'); self.assertTrue(any('missing series' in p for p in v['problems']))

    def test_consistent_fail_is_a_valid_fail_not_invalid(self):
        b = Board(sse_old='FAIL'); code, rep, _ = run(b, {'add': ['--route', 'sse']})
        self.assertEqual(rep['sseOld']['overall'], 'FAIL'); self.assertNotIn('problems', rep['sseOld']); self.assertEqual(code, 3)

    def test_stale_result_file_is_not_reused(self):
        b = Board(); real = b.run_child
        def child(cmd, path, timeout, cwd):
            if any(str(c).endswith('e1_sse_diag.py') for c in cmd):
                self.assertFalse(Path(cmd[cmd.index('--out') + 1]).exists(), 'old result removed before the run')
                return 'timeout'  # writes nothing
            return real(cmd, path, timeout, cwd)
        b.run_child = child
        code, rep, _ = run(b, {'add': ['--route', 'sse']})
        self.assertIn('not measured', rep['sseOld']); self.assertEqual(code, 3)

    def test_preflight_and_dry_run_unchanged_for_sse_route(self):
        b = Board(master=True); code, rep, _ = run(b, {'add': ['--route', 'sse']})
        self.assertEqual(code, 2); self.assertEqual(changes(b), ([], []))
        b = Board(); code, rep, _ = run(b, {'add': ['--route', 'sse', '--dry-run']})
        self.assertEqual(code, 0); self.assertEqual(changes(b), ([], []))

    def test_signal_during_the_new_diagnostic_restores_everything(self):
        b = Board(); real = b.run_child
        def child(cmd, *a):
            if any(str(c).endswith('e1_sse_diag.py') for c in cmd) and b.image == 'new': raise KeyboardInterrupt('signal 15')
            return real(cmd, *a)
        b.run_child = child
        code, rep, _ = run(b, {'add': ['--route', 'sse']})
        self.assertIn('signal 15', rep['interrupted']); self.assertEqual(code, 3); self.assertRestoredOld(b, rep)

    def test_worst_case_durations_stay_within_session_limit(self):
        class Slow(Board):
            def run(self, cmd, timeout=30):
                t0 = self.t; res = super().run(cmd, timeout); self.t += max(0, timeout - (self.t - t0)); return res
            def run_child(self, cmd, path, timeout, cwd):
                t0 = self.t; res = super().run_child(cmd, path, timeout, cwd); self.t += max(0, timeout - (self.t - t0)); return res
            def usb_status(self, h):
                self.t += 8; return super().usb_status(h)
        for extra in ({}, {'rejoin': 'hang', 'autojoin': False}):
            with self.subTest(**extra):
                b = Slow(**extra); code, rep, _ = run(b, {'add': ['--route', 'sse']})
                self.assertLessEqual(rep['sessionSeconds'], session.SESSION_LIMIT, rep['steps'][-5:])
                self.assertNotEqual(code, 0)


class Clock:
    """Raw system for Budget tests: records timeouts it was given; time moves only on sleep.
    A loop guard turns a wait that never ends into a test failure instead of a hang."""
    def __init__(self):
        self.t, self.timeouts, self.calls = 0.0, [], 0
    def now(self):
        self.calls += 1
        if self.calls > 10000: raise AssertionError('wait did not stop at the phase deadline')
        return self.t
    def sleep(self, s): self.t += s
    def run(self, cmd, timeout=30): self.timeouts.append(timeout); return 0, ''
    def run_child(self, cmd, path, timeout, cwd): self.timeouts.append(timeout); return 0
    def usb_status(self, h): self.timeouts.append('status'); return {}
    def usb_cmd(self, h, obj, timeout=8): self.timeouts.append(timeout); return {}


class BudgetProxy(unittest.TestCase):
    def test_blocking_calls_are_clamped_to_the_phase(self):
        raw = Clock(); b = session.Budget(raw)
        with b.phase(10):
            b.run(['x'], timeout=60); b.run_child(['x'], 'p', 300, '.'); b.usb_cmd('h', {}); b.sleep(30)
        self.assertEqual(raw.timeouts, [10, 10, 8]); self.assertEqual(raw.t, 10)
        with b.phase(5):
            self.assertIsNone(b.usb_status('h'), 'a status read may take 8 s, only 5 s are left')
            b.usb_cmd('h', {}); self.assertEqual(raw.timeouts[-1], 5)
        with b.phase(0):
            self.assertEqual(b.run(['x']), (124, 'no time left')); self.assertEqual(b.run_child(['x'], 'p', 9, '.'), 'no time left')
        self.assertEqual(len(raw.timeouts), 4, 'nothing was called without time')

    def test_phases_nest_and_restore(self):
        raw = Clock(); b = session.Budget(raw); b.until = 100
        with b.phase(500):
            self.assertEqual(b.until, 100, 'an inner phase cannot extend the outer one')
            with b.phase(until=40): self.assertEqual(b.until, 40)
            self.assertEqual(b.until, 100)
        self.assertEqual(b.until, 100)

    def test_waits_stop_at_the_phase_deadline(self):
        raw = Clock(); b = session.Budget(raw)
        with b.phase(20):
            self.assertIsNone(session.ap_run.wait_for(b, lambda: None, 3600, step=3))
        self.assertLessEqual(raw.t, 20)


if __name__ == '__main__':
    unittest.main()
