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
    def exists(self, path): return self.f['port']
    def hold_port(self, port):
        if self.f['hold_fail']: raise OSError('no such port')
        return super().hold_port(port)
    def usb_status(self, h):
        st = super().usb_status(h)
        if not self.f['settle']: st['otaConfirmed'] = False
        st.update(firmware='0.4.0+' + (OLD_SHA if self.image == 'old' else NEW_SHA)[:7], bootId=f'boot{self.boot}', master=self.f['master'],
                  channels=[{'on': self.f['output_on'] and i == 0, 'data': False} for i in range(8)])
        return st
    def usb_cmd(self, h, obj, timeout=8):
        return {'deviceId': self.f['device']} if obj.get('cmd') == 'info' else None
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


if __name__ == '__main__':
    unittest.main()
