"""Offline tests of e1_ap_run.py: every system command is simulated; no network change."""
import json, os, stat, tempfile, unittest
from pathlib import Path
import e1_ap_run as runner

AP = runner.ap_credentials()[0]
HOME_GW, AP_GW = '10.0.0.1', '192.168.4.1'


class FakeSystem:
    """network: 'home' | 'ap' | None. Flags simulate failures."""
    def __init__(self, **f):
        self.network, self.t, self.calls = 'home', 0.0, []
        self.preferred = ['HomeNet', 'Other']
        self.f = dict(join_ap_ok=True, rejoin='ok', autojoin=True, forget_ok=True, child=0, ap_addr=True, pyserial=True, route_raises=False, browser=0, usb_backup=0, usb_restore=0)
        self.f.update(f)
    def hold_port(self, port):
        self.calls.append(['hold', port]); self.held = True; return 'handle'
    def release_port(self, h): self.calls.append(['release']); self.held = False
    def usb_status(self, h): return {'otaConfirmed': True, 'ap': True, 'wifi': {'phase': self.f.get('board_phase', 'ap_only')}, 'firmware': 'f', 'bootId': 'b'}
    def now(self): return self.t
    def sleep(self, s): self.t += s
    def run(self, cmd, timeout=30):
        self.calls.append(cmd); c = cmd[0]
        if c == 'route':
            if self.f['route_raises'] and self.network == 'ap': raise RuntimeError('route tool crashed')
            gw = HOME_GW if self.network == 'home' else AP_GW if self.network == 'ap' else None
            return (0, f'interface: en0\ngateway: {gw}') if gw else (1, 'not in table')
        if c == 'ipconfig':
            if self.network == 'home': return 0, '10.0.0.50'
            if self.network == 'ap' and self.f['ap_addr']: return 0, '192.168.4.2'
            return 1, ''
        if c == 'networksetup':
            op = cmd[1]
            if op == '-listpreferredwirelessnetworks': return 0, 'Preferred networks on en0:\n' + '\n'.join('\t' + p for p in self.preferred)
            if op == '-setairportnetwork':
                if cmd[3] == AP:
                    if self.f['join_ap_ok']: self.network = 'ap'; self.preferred.append(AP)
                    return 0, ''
                r = self.f['rejoin']
                if r == 'hang': self.t += timeout; return 124, 'timeout'
                if r == 'fail': return 1, 'Could not find network'
                self.network = 'home'; return 0, ''
            if op == '-setairportpower':
                if cmd[3] == 'off': self.network = None
                elif self.f['autojoin']: self.network = 'home'
                return 0, ''
            if op == '-removepreferredwirelessnetwork':
                if self.f['forget_ok'] and cmd[3] in self.preferred: self.preferred.remove(cmd[3])
                return (0, '') if self.f['forget_ok'] else (1, 'failed')
        if c == 'ping': return (0, '') if self.network == 'home' and cmd[-1] == HOME_GW else (1, '')
        if cmd[1:3] == ['-c', 'import serial']: return (0, '') if self.f['pyserial'] else (1, 'No module')
        if len(cmd) > 2 and cmd[1].endswith('e1_usb_config.py'): return self.f['usb_' + cmd[2]], cmd[2]
        if cmd[0] == 'node': return 0, 'v24'
        return 0, ''
    def run_child(self, cmd, stdout_path, timeout, cwd):
        self.calls.append(['child'] + cmd)
        Path(stdout_path).write_text('child output\n')
        key = 'browser' if any(str(c).endswith('e1_browser.mjs') for c in cmd) else 'child'
        if self.f[key] == 'timeout': self.t += timeout; return 'timeout'
        return self.f[key]


def run(fs, extra=()):
    d = tempfile.mkdtemp()
    code = runner.main(['--port', 'P', '--return-ssid', 'HomeNet', '--python', 'py', *extra], sy=fs, out_dir=d)
    files = list(Path(d).glob('*-e1-run.json'))
    report = json.loads(files[0].read_text()) if files else None
    return code, report, d


def net_changes(fs): return [c for c in fs.calls if c[0] == 'networksetup' and c[1] in ('-setairportnetwork', '-setairportpower', '-removepreferredwirelessnetwork')]


class E1Runner(unittest.TestCase):
    def test_success(self):
        fs = FakeSystem(); code, rep, _ = run(fs)
        self.assertEqual(code, 0); self.assertTrue(rep['e1'].startswith('PASS')); self.assertTrue(rep['restore']['confirmed']); self.assertTrue(rep['restore']['apForgotten'])
        self.assertEqual(fs.network, 'home')

    def test_rejoin_fails_while_ap_route_and_ping_alive_is_not_confirmed_without_fallback(self):
        fs = FakeSystem(rejoin='fail'); code, rep, _ = run(fs)
        steps = [s['step'] for s in rep['steps']]
        self.assertIn('power-off', steps)  # an AP route on en0 did not count as "back"
        self.assertTrue(rep['restore']['confirmed']); self.assertEqual(code, 0)

    def test_rejoin_and_fallback_fail_reports_unconfirmed(self):
        fs = FakeSystem(rejoin='fail', autojoin=False); code, rep, _ = run(fs)
        self.assertFalse(rep['restore']['confirmed']); self.assertEqual(code, 4)

    def test_rejoin_hangs_then_fallback_continues(self):
        fs = FakeSystem(rejoin='hang'); code, rep, _ = run(fs)
        self.assertIn('power-on', [s['step'] for s in rep['steps']]); self.assertTrue(rep['restore']['confirmed']); self.assertEqual(code, 0)

    def test_forget_failure_is_reported(self):
        fs = FakeSystem(forget_ok=False); code, rep, _ = run(fs)
        self.assertFalse(rep['restore']['apForgotten']); self.assertTrue(rep['restore']['confirmed'])

    def test_child_failure_timeout_and_skip_are_nonzero(self):
        self.assertEqual(run(FakeSystem(child=7))[0], 3)
        code, rep, _ = run(FakeSystem(child='timeout')); self.assertEqual(code, 3); self.assertIn('timeout', rep['e1'])
        code, rep, _ = run(FakeSystem(ap_addr=False)); self.assertEqual(code, 3); self.assertIn('no AP address', rep['e1'])

    def test_both_failures_reported(self):
        code, rep, _ = run(FakeSystem(child=7, rejoin='fail', autojoin=False))
        self.assertEqual(code, 5); self.assertIn('FAIL', rep['e1']); self.assertFalse(rep['restore']['confirmed'])

    def test_cleanup_exception_still_saves_report(self):
        fs = FakeSystem(route_raises=True, rejoin='fail', autojoin=False); code, rep, _ = run(fs)
        self.assertIsNotNone(rep); self.assertFalse(rep['restore']['confirmed']); self.assertNotEqual(code, 0)

    def test_dry_run_and_refused_preflight_change_nothing(self):
        fs = FakeSystem(); code, rep, _ = run(fs, ['--dry-run'])
        self.assertEqual(code, 0); self.assertEqual(net_changes(fs), []); self.assertIn('NOT RUN', rep['e1'])
        fs = FakeSystem(pyserial=False); code, rep, _ = run(fs); self.assertEqual(code, 2); self.assertEqual(net_changes(fs), [])
        fs = FakeSystem(); code = runner.main(['--port', 'P', '--return-ssid', 'Unknown', '--python', 'py'], sy=fs, out_dir=tempfile.mkdtemp())
        self.assertEqual(code, 2); self.assertEqual(net_changes(fs), [])

    def test_private_files_are_0600(self):
        old = os.umask(0o022)
        try:
            fs = FakeSystem(); code, rep, d = run(fs)
            for p in Path(d).iterdir():
                self.assertEqual(stat.S_IMODE(p.stat().st_mode) & 0o077, 0, p.name)
        finally: os.umask(old)

    def test_interrupt_runs_restore(self):
        fs = FakeSystem()
        def boom(*a, **k): raise KeyboardInterrupt('signal 15')
        fs.run_child = boom
        code, rep, _ = run(fs)
        self.assertIn('interrupted', rep['e1']); self.assertTrue(rep['restore']['confirmed']); self.assertEqual(code, 3); self.assertEqual(fs.network, 'home')


class E1RunnerBrowser(unittest.TestCase):
    def test_browser_success_restores_config_then_network(self):
        fs = FakeSystem(); code, rep, _ = run(fs, ['--browser'])
        self.assertEqual(code, 0); self.assertEqual(rep['browser'], 'PASS'); self.assertEqual(rep['configRestore'], 'CONFIRMED')
        steps = [s['step'] for s in rep['steps']]
        self.assertLess(steps.index('hold-port'), steps.index('usb-backup')); self.assertFalse(fs.held)
        self.assertLess(steps.index('usb-backup'), steps.index('join-ap')); self.assertLess(steps.index('usb-config-restore'), steps.index('rejoin'))

    def test_browser_failure_is_nonzero_and_still_restores(self):
        fs = FakeSystem(browser=1); code, rep, _ = run(fs, ['--browser'])
        self.assertEqual(code, 3); self.assertEqual(rep['configRestore'], 'CONFIRMED'); self.assertTrue(rep['restore']['confirmed'])

    def test_config_restore_failure_counts_as_unconfirmed_restore(self):
        fs = FakeSystem(usb_restore=1); code, rep, _ = run(fs, ['--browser'])
        self.assertEqual(code, 4); self.assertIn('NOT confirmed', rep['configRestore'])

    def test_unsettled_board_refuses_before_switching(self):
        fs = FakeSystem(board_phase='associating'); code, rep, _ = run(fs, ['--browser'])
        self.assertEqual(code, 2); self.assertEqual(net_changes(fs), []); self.assertFalse(fs.held)

    def test_backup_failure_refuses_before_switching(self):
        fs = FakeSystem(usb_backup=1); code, rep, _ = run(fs, ['--browser'])
        self.assertEqual(code, 2); self.assertEqual(net_changes(fs), [])


if __name__ == '__main__':
    unittest.main()
