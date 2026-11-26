#!/usr/bin/env python3
"""One bounded E1/WDT hardware session, unattended (start detached, e.g. `nohup ... &`).

Route:
  preflight   nothing is changed: USB port, board identity (deviceId, firmware = old image),
              Master and every channel's output and DATA proven off (8 channels, explicit
              false values; anything missing or malformed refuses), verified image manifests
              (old 09e092a without the Stream yield, new with it), Python/node/esptool,
              Wi-Fi interface + original gateway, return SSID preferred AND --return-confirmed
              (the user confirmed that network works now). Any doubt -> exit 2, NOT RUN.
  backup      configuration over USB into a private file (0600), read back and compared.
  old         join the board AP, e1_partial_request.py on the old image (reset expected).
  flash new   release the port, esptool writes the new image set (bootloader, partitions,
              boot_app0, app; the config filesystem is not touched), board settles,
              firmware = new image, same deviceId, configuration unchanged.
  new         rejoin the AP, the same minimal case (same boot + measured HTTP recovery expected).
  e1          e1_ap_run.run_checks: check_p1c_http.py --ap and e1_browser.mjs.
  cleanup     always, also after errors, timeouts and SIGTERM/SIGHUP/Ctrl-C (ignored while it
              runs): image policy (keep the new image only if the old reset was reproduced,
              the new image did not reset and HTTP recovered, and E1 passed; otherwise flash the
              old image back), configuration restore over USB (confirmed), network restore
              (e1_ap_run.restore, attempted whenever a join may have changed the network),
              port release.

Time: every blocking operation goes through Budget, which clamps its timeout (commands,
child processes, USB reads, sleeps and the waits inside e1_ap_run) to the deadline of the
current phase. Phases: main steps end by START + SESSION_LIMIT - CLEANUP_RESERVE and a step
starts only if its whole limit fits; cleanup has sub-deadlines (rollback, then configuration,
then network) so the session ends by START + SESSION_LIMIT. A step cut by its deadline is
reported as not confirmed, never as success.
Results are reported separately; nothing is merged into one PASS. The JSON report is
private (0600). Passwords and tokens are never written.
Exit: 0 all passed and cleanup confirmed; 2 preflight refused (nothing changed);
3 a check failed / not measured, cleanup confirmed; 4 cleanup not confirmed; 5 both."""
import argparse, contextlib, json, os, signal, sys, time
from pathlib import Path

import e1_ap_run as ap_run
import e1_images

ROOT = ap_run.ROOT
SESSION_LIMIT = 25 * 60
USB_READ = 8
CHANNELS = 8
# Whole-step limits, including everything inside the step (sleeps, polls, USB reads).
LIMITS = {'settle': 150, 'usb-config': 60, 'join': 90, 'partial': 75, 'flash': 120}
# Cleanup sub-budgets (s): rollback = flash + settle; configuration restore; network restore.
ROLLBACK_RESERVE = LIMITS['flash'] + LIMITS['settle']
CONFIG_RESERVE = 75
NETWORK_RESERVE = 330
CLEANUP_RESERVE = ROLLBACK_RESERVE + CONFIG_RESERVE + NETWORK_RESERVE
# E1 gets what is left before the main deadline, split HTTP/browser, with these bounds (s).
E1_HTTP = (120, 300)
E1_BROWSER = (90, 240)
ESPTOOL = Path.home() / '.platformio/packages/tool-esptoolpy/esptool.py'


class System(ap_run.System):
    def usb_cmd(self, handle, obj, timeout=USB_READ):
        handle.reset_input_buffer(); handle.write((json.dumps(obj) + '\n').encode()); handle.flush()
        end = time.monotonic() + timeout; raw = b''
        while time.monotonic() < end:
            raw += handle.readline()
            if raw.endswith(b'\n'):
                line = raw.decode(errors='replace').strip(); raw = b''
                if line.startswith('L8 {'): return json.loads(line[3:])
        return None
    def exists(self, path): return Path(path).exists()


class Budget:
    """System proxy: each blocking call is clamped to the current phase deadline `until`."""
    def __init__(self, sy):
        self.sy, self.until = sy, float('inf')
    def now(self): return self.sy.now()
    def left(self): return self.until - self.sy.now()
    def expired(self): return self.left() <= 0
    def sleep(self, s):
        if self.left() > 0: self.sy.sleep(min(s, self.left()))
    def run(self, cmd, timeout=30):
        if self.left() <= 0: return 124, 'no time left'
        return self.sy.run(cmd, timeout=min(timeout, self.left()))
    def run_child(self, cmd, path, timeout, cwd):
        if self.left() <= 0: return 'no time left'
        return self.sy.run_child(cmd, path, min(timeout, self.left()), cwd)
    def usb_status(self, h):
        return self.sy.usb_status(h) if self.left() >= USB_READ else None  # one read may take USB_READ
    def usb_cmd(self, h, obj):
        return self.sy.usb_cmd(h, obj, timeout=min(USB_READ, self.left())) if self.left() >= 1 else None
    def hold_port(self, port): return self.sy.hold_port(port)
    def release_port(self, h): return self.sy.release_port(h)
    def exists(self, path): return self.sy.exists(path)
    @contextlib.contextmanager
    def phase(self, seconds=None, until=None):
        old = self.until
        end = until if until is not None else self.now() + seconds
        self.until = min(old, end)
        try: yield
        finally: self.until = old


def outputs_proven_off(st, channels=CHANNELS):
    """Master and every channel's output and DATA explicitly false; anything else is not proof."""
    ch = st.get('channels') if isinstance(st, dict) else None
    return (isinstance(st, dict) and st.get('master') is False and isinstance(ch, list) and len(ch) == channels
            and all(isinstance(c, dict) and c.get('on') is False and c.get('data') is False for c in ch))


def classify_old(code):
    return {1: 'reproduced (bootId changed)', 0: 'not reproduced (same bootId)'}.get(code, f'not measured ({code})')


def classify_new(code, recovery):
    if code == 0 and recovery is not None: return True, f'no reset, HTTP recovered in {recovery} s'
    if code == 0: return False, 'no reset, but HTTP did not recover'
    if code == 1: return False, 'RESET (bootId changed)'
    return False, f'not measured ({code})'


def main(argv=None, sy=None, out_dir=None):
    raw = sy or System()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True)
    ap.add_argument('--device-id', required=True, help='exact deviceId of the board (USB info)')
    ap.add_argument('--old', required=True, help='manifest.json of the old image set')
    ap.add_argument('--new', required=True, help='manifest.json of the new image set')
    ap.add_argument('--return-ssid', required=True)
    ap.add_argument('--return-confirmed', action='store_true', help='the user confirmed the return network works now')
    ap.add_argument('--iface', default='en0')
    ap.add_argument('--python', default=str(ap_run.DEFAULT_PYTHON if ap_run.DEFAULT_PYTHON.exists() else sys.executable))
    ap.add_argument('--node', default='node')
    ap.add_argument('--esptool', default=str(ESPTOOL))
    ap.add_argument('--dry-run', action='store_true', help='preflight only, then stop')
    args = ap.parse_args(argv)
    os.umask(0o077)
    sy = Budget(raw)
    start = sy.now(); deadline = start + SESSION_LIMIT; main_deadline = deadline - CLEANUP_RESERVE
    sy.until = main_deadline
    stamp = time.strftime('%Y-%m-%d-%H%M%S')
    out_dir = Path(out_dir or ROOT / 'logs/diagnostics'); out_dir.mkdir(parents=True, exist_ok=True)
    report = {'stamp': stamp, 'steps': [], 'oldWdt': 'not measured', 'newImage': 'not measured', 'usbHttpSse': 'NOT RUN',
              'browser': 'NOT RUN', 'configRestore': 'not needed', 'networkRestore': 'not needed', 'imageLeft': 'old (unchanged)'}
    def log(name, **kw):
        report['steps'].append({'step': name, 't': round(sy.now() - start, 1), **kw}); print(name, kw, flush=True)
    def save():
        fd = os.open(out_dir / f'{stamp}-e1-session.json', os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, 'w') as f: json.dump(report, f, indent=1)
    def fits(*steps):
        ok = sy.now() + sum(LIMITS[x] for x in steps) <= main_deadline
        if not ok: log('skip-no-time', steps=list(steps))
        return ok
    def child(name, cmd, limit, cwd=ROOT / 'scripts'):
        path = out_dir / f'{stamp}-{name}.txt'
        rc = sy.run_child(cmd, str(path), limit, str(cwd)); log(name, exit=rc, stdout=path.name); return rc, path
    def usb_cfg(mode, backup):
        return sy.run([args.python, str(ROOT / 'scripts/e1_usb_config.py'), mode, '--port', args.port, '--file', str(backup)],
                      timeout=LIMITS['usb-config'])
    state = {'network': False, 'flashed': False, 'handle': None}
    def board_ready():
        st = sy.usb_status(state['handle'])  # always the currently open handle
        return st if st and st.get('otaConfirmed') and st.get('ap') and st.get('wifi', {}).get('phase') in ('online', 'ap_only') else None
    def settle():
        """(status, info) from the currently held port once Wi-Fi settled, within LIMITS['settle']."""
        with sy.phase(LIMITS['settle']):
            sy.sleep(12)
            st = ap_run.wait_for(sy, board_ready, LIMITS['settle'], step=3)
            return st, (sy.usb_cmd(state['handle'], {'cmd': 'info'}) if st else None)

    # ---- preflight: read only ----
    ap_ssid, ap_pwd = ap_run.ap_credentials()
    iface0, gw0 = ap_run.default_route(sy)
    known = ap_run.preferred(sy, args.iface) or []
    old_img_ok, old_why, old_m = e1_images.verify(args.old)
    new_img_ok, new_why, new_m = e1_images.verify(args.new)
    roles_ok = bool(old_m and new_m and old_m.get('role') == 'old' and new_m.get('role') == 'new')
    tools = {'pyserial': sy.run([args.python, '-c', 'import serial'], timeout=20)[0] == 0,
             'node': sy.run([args.node, '--version'], timeout=20)[0] == 0,
             'esptool': sy.run([args.python, args.esptool, 'version'], timeout=30)[0] == 0}
    net_ok = iface0 == args.iface and bool(gw0) and not gw0.startswith(ap_run.AP_PREFIX) and args.return_ssid in known
    pre = dict(port=sy.exists(args.port), images=old_img_ok and new_img_ok and roles_ok, imageProblems=old_why + new_why,
               tools=tools, network=net_ok, returnConfirmed=args.return_confirmed, apKnownBefore=ap_ssid in known)
    ok = pre['port'] and pre['images'] and all(tools.values()) and net_ok and args.return_confirmed
    log('preflight-static', **pre, ok=ok)
    if not ok:
        report['result'] = 'NOT RUN (preflight refused, nothing changed)'; save(); return 2
    old_ver = '0.4.0+' + old_m['sourceCommit'][:7]; new_ver = '0.4.0+' + new_m['sourceCommit'][:7]
    try:
        state['handle'] = sy.hold_port(args.port)
    except Exception as e:
        log('hold-port', error=repr(e)); report['result'] = 'NOT RUN (USB port unavailable, nothing changed)'; save(); return 2
    def refuse(why):
        sy.release_port(state['handle']); report['result'] = f'NOT RUN ({why}, nothing changed)'; save(); return 2
    try:
        st, info = settle()
    except Exception as e:
        log('preflight-board', error=repr(e)); return refuse('board not readable')
    limits = (info or {}).get('limits')
    board = dict(settled=bool(st), deviceIdMatches=bool(info) and info.get('deviceId') == args.device_id,
                 channelsDeclared=limits.get('channels') if isinstance(limits, dict) else None,
                 firmware=st and st.get('firmware'), firmwareIsOld=bool(st) and st.get('firmware') == old_ver,
                 outputsOff=outputs_proven_off(st))
    log('preflight-board', **board)
    if not (board['settled'] and board['deviceIdMatches'] and board['firmwareIsOld'] and board['outputsOff']
            and board['channelsDeclared'] in (None, CHANNELS)):
        return refuse('board identity/state not proven')
    backup = out_dir / f'{stamp}-e1-config-backup.json'
    code, _ = usb_cfg('backup', backup); vcode = usb_cfg('verify', backup)[0] if code == 0 else None
    log('usb-backup', code=code, verify=vcode)
    if code != 0 or vcode != 0:
        return refuse('configuration backup not verified')
    if args.dry_run:
        sy.release_port(state['handle']); report['result'] = 'NOT RUN (dry-run: preflight passed)'; save(); return 0

    # ---- session ----
    def on_signal(signum, frame): raise KeyboardInterrupt(f'signal {signum}')
    for s in (signal.SIGTERM, signal.SIGHUP):
        try: signal.signal(s, on_signal)
        except (ValueError, OSError): pass
    def join_ap():
        if not fits('join'): return False
        # Mark first: the network may change even if the command does not return (signal, timeout).
        state['network'] = True; report['networkRestore'] = 'pending'
        with sy.phase(LIMITS['join']):
            code, out = sy.run(['networksetup', '-setairportnetwork', args.iface, ap_ssid, ap_pwd], timeout=60)
            addr = ap_run.wait_for(sy, lambda: (lambda a: a if a and a.startswith(ap_run.AP_PREFIX) else None)(ap_run.address(sy, args.iface)),
                                   LIMITS['join'])
        log('join-ap', code=code, output=out.replace(ap_pwd, '<pw>')[:120], address=addr); return bool(addr)
    def partial(name):
        if not fits('partial'): return None, None
        with sy.phase(LIMITS['partial']):
            rc, path = child(name, [args.python, str(ROOT / 'scripts/e1_partial_request.py'), '--port', args.port], LIMITS['partial'])
        try: res = json.loads(Path(path).read_text().strip().splitlines()[-1])
        except (OSError, ValueError, IndexError): res = {}
        return rc, res.get('httpRecoveryS')
    def flash(manifest_path, m):
        if state['handle'] is not None:
            sy.release_port(state['handle']); state['handle'] = None
        with sy.phase(LIMITS['flash']):
            cmd = [args.python, args.esptool, '--port', args.port] + e1_images.flash_args(m, Path(manifest_path).parent)
            code, out = sy.run(cmd, timeout=LIMITS['flash'])
        log('flash', role=m['role'], code=code, output=out[-160:]); return code == 0
    def settle_as(version):
        state['handle'] = sy.hold_port(args.port)
        st, i = settle()
        ok = bool(st) and st.get('firmware') == version and bool(i) and i.get('deviceId') == args.device_id
        log('settled', expected=version, firmware=st and st.get('firmware'), ok=ok); return ok

    new_ok = e1_ok = old_repro = False
    try:
        if join_ap():
            rc, _ = partial('partial-old'); report['oldWdt'] = classify_old(rc); old_repro = rc == 1
            if fits('flash', 'settle'):
                state['flashed'] = True  # from here the board may hold the new image
                if flash(args.new, new_m) and settle_as(new_ver):
                    report['imageLeft'] = 'new (pending policy)'
                    vcode = usb_cfg('verify', backup)[0] if fits('usb-config') else 'skipped'
                    log('config-after-flash', same=vcode == 0)
                    if vcode == 0 and join_ap():
                        rc, rec = partial('partial-new'); new_ok, report['newImage'] = classify_new(rc, rec)
                        left = main_deadline - sy.now()
                        browser_s = min(E1_BROWSER[1], int(left * 0.4)); http_s = min(E1_HTTP[1], int(left - browser_s))
                        if http_s >= E1_HTTP[0] and browser_s >= E1_BROWSER[0]:
                            e1 = {}
                            log('e1-budget', http=http_s, browser=browser_s)
                            with sy.phase(http_s + browser_s):
                                e1_ok = ap_run.run_checks(sy, args.python, args.port, args.node, http_s, browser_s, out_dir, stamp, e1, log)
                            report['usbHttpSse'] = e1.get('http', 'NOT RUN'); report['browser'] = e1.get('browser', 'NOT RUN')
                        else:
                            log('skip-no-time', steps=['http', 'browser'], left=round(left))
                            report['usbHttpSse'] = report['browser'] = 'NOT RUN (no time left in the session)'
                    elif vcode != 0:
                        report['newImage'] = f'not measured (configuration after flash not verified: {vcode})'
                else:
                    report['newImage'] = 'not measured (flash or settle failed)'
    except BaseException as e:
        log('interrupted', error=repr(e)); report['interrupted'] = repr(e)
    finally:
        for s in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
            try: signal.signal(s, signal.SIG_IGN)
            except (ValueError, OSError): pass
        sy.until = deadline
        cleanup_ok = cleanup(sy, args, state, report, log, old_repro and new_ok and e1_ok, flash, settle_as, old_ver, backup,
                             usb_cfg, gw0, ap_ssid, ap_ssid in known, deadline)
        report['sessionSeconds'] = round(sy.now() - start, 1); save()
    tests_ok = old_repro and new_ok and e1_ok
    report['result'] = 'PASS' if tests_ok and cleanup_ok else 'see fields'
    save()
    return 0 if tests_ok and cleanup_ok else 5 if not tests_ok and not cleanup_ok else 4 if not cleanup_ok else 3


def cleanup(sy, args, state, report, log, keep_new, flash, settle_as, old_ver, backup, usb_cfg, gw0, ap_ssid, ap_known, deadline):
    """Each part has its own sub-deadline, so a slow part cannot eat the later ones."""
    def step(name, fn):
        try: return fn()
        except BaseException as e: log(name, error=repr(e)); return None
    image_ok = True
    with sy.phase(until=deadline - CONFIG_RESERVE - NETWORK_RESERVE):
        if state['flashed']:
            if keep_new:
                report['imageLeft'] = 'new (comparison and E1 passed)'
            else:
                back = step('rollback-flash', lambda: flash(args.old, json.loads(Path(args.old).read_text())) and settle_as(old_ver))
                image_ok = bool(back)
                log('rollback', confirmed=image_ok)
                report['imageLeft'] = 'old (rolled back)' if back else 'UNKNOWN (rollback to old image NOT confirmed)'
    with sy.phase(until=deadline - NETWORK_RESERVE):
        if state['handle'] is None:
            def rehold():
                state['handle'] = sy.hold_port(args.port); sy.sleep(12)
            step('re-hold-port', rehold)
        code = step('usb-config-restore', lambda: usb_cfg('restore', backup)[0])
        report['configRestore'] = 'CONFIRMED' if code == 0 else f'NOT confirmed ({code})'
        log('usb-config-restore', result=report['configRestore'])
    with sy.phase(until=deadline):
        if state['network']:
            res = step('network-restore', lambda: ap_run.restore(sy, args.iface, args.return_ssid, gw0, ap_ssid, ap_known, log))
            report['networkRestore'] = res or {'confirmed': False}
            log('network-restore', confirmed=report['networkRestore'].get('confirmed'))
        if state['handle'] is not None: step('release-port', lambda: sy.release_port(state['handle']))
    net_ok = not state['network'] or bool(report['networkRestore'].get('confirmed'))
    return image_ok and code == 0 and net_ok


if __name__ == '__main__':
    sys.exit(main())
