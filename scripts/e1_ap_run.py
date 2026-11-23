#!/usr/bin/env python3
"""Bounded, unattended E1 run over the board's AP, then a bounded network restore.

Runs locally (start detached, e.g. `nohup ... &`): it must not depend on an agent or a
chat while the computer has no internet.

  1. preflight: Wi-Fi interface on the default route, original gateway recorded,
     --return-ssid present in the preferred list (needed because macOS redacts the
     current SSID; being preferred does NOT prove it is the current network or that
     the stored password works - that stays the user's confirmation), and the chosen
     Python can import pyserial;
  2. join the board AP (SSID/password from src/config/config.h, never logged);
  3. wait for a 192.168.4.x address;
  4. run scripts/check_p1c_http.py --ap under a hard time budget;
  5. restore, every step bounded and isolated (one failing/hanging step does not stop
     the others): rejoin --return-ssid; if the ORIGINAL gateway is not back on the
     interface (an AP route on the same interface does not count), power-cycle Wi-Fi
     for macOS auto-join; forget ZVER-L8 if it was not known before and verify that;
     ping the original gateway.
  6. the JSON report is written even if cleanup raises; SIGTERM/SIGHUP/Ctrl-C run the
     restore too. Private files are created 0600 (umask 077).

Exit status: 0 = test passed AND restore confirmed; 3 = test failed/timed out/skipped;
4 = restore not confirmed; 5 = both; 2 = preflight refused (network untouched).
--dry-run never changes the network; its report says E1 NOT RUN.
No absolute guarantee is possible if Wi-Fi itself fails; then the report says so."""
import argparse, json, os, re, signal, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AP_PREFIX = '192.168.4.'
DEFAULT_PYTHON = Path.home() / '.platformio/penv/bin/python'


class System:
    """Real system access. Tests replace it; every call is bounded and never raises."""
    def run(self, cmd, timeout=30):
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
            return r.returncode, (r.stdout + r.stderr).strip()
        except subprocess.TimeoutExpired:
            return 124, 'timeout'
        except OSError as e:
            return 127, str(e)
    def run_child(self, cmd, stdout_path, timeout, cwd):
        fd = os.open(stdout_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, 'w') as f:
            try: return subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, timeout=timeout, cwd=cwd).returncode
            except subprocess.TimeoutExpired: return 'timeout'
            except OSError as e: return f'error: {e}'
    def now(self): return time.monotonic()
    def sleep(self, s): time.sleep(s)


def ap_credentials():
    text = (ROOT / 'src/config/config.h').read_text()
    return (re.search(r'#define\s+AP_SSID\s+"([^"]*)"', text).group(1),
            re.search(r'#define\s+AP_PASSWORD\s+"([^"]*)"', text).group(1))


def default_route(sy):
    code, out = sy.run(['route', '-n', 'get', 'default'])
    i = re.search(r'interface:\s*(\S+)', out); g = re.search(r'gateway:\s*(\S+)', out)
    return (i.group(1) if i else None), (g.group(1) if g else None)


def address(sy, iface):
    code, out = sy.run(['ipconfig', 'getifaddr', iface])
    return out.strip() if code == 0 else None


def preferred(sy, iface):
    code, out = sy.run(['networksetup', '-listpreferredwirelessnetworks', iface])
    return [l.strip() for l in out.splitlines()[1:]] if code == 0 else None


def wait_for(sy, pred, seconds, step=2):
    end = sy.now() + seconds
    while True:
        try:
            v = pred()
        except Exception:
            v = None
        if v: return v
        if sy.now() >= end: return None
        sy.sleep(step)


def original_back(sy, iface, gw0):
    """The original gateway is the default route again on this interface and the
    interface is not on the board AP subnet. A route via the AP does not count."""
    i, g = default_route(sy); a = address(sy, iface)
    return i == iface and g == gw0 and not (g or '').startswith(AP_PREFIX) and not (a or '').startswith(AP_PREFIX)


def restore(sy, iface, return_ssid, gw0, ap_ssid, ap_known_before, log):
    result = {'confirmed': False}
    def attempt(name, fn):
        try:
            v = fn(); log(name, result=v); return v
        except Exception as e:  # keep going with the remaining steps
            log(name, error=repr(e)); return None
    attempt('rejoin', lambda: sy.run(['networksetup', '-setairportnetwork', iface, return_ssid], timeout=60))
    back = attempt('wait-original', lambda: bool(wait_for(sy, lambda: original_back(sy, iface, gw0), 45)))
    if not back:
        attempt('power-off', lambda: sy.run(['networksetup', '-setairportpower', iface, 'off'], timeout=20))
        sy.sleep(3)
        attempt('power-on', lambda: sy.run(['networksetup', '-setairportpower', iface, 'on'], timeout=20))
        back = attempt('wait-original-after-power-cycle', lambda: bool(wait_for(sy, lambda: original_back(sy, iface, gw0), 90)))
    if not ap_known_before:
        attempt('forget-ap', lambda: sy.run(['networksetup', '-removepreferredwirelessnetwork', iface, ap_ssid], timeout=20))
        listed = attempt('verify-forget', lambda: preferred(sy, iface))
        result['apForgotten'] = listed is not None and ap_ssid not in listed
    else:
        result['apForgotten'] = 'not needed (known before)'
    ping = attempt('ping-original-gateway', lambda: sy.run(['ping', '-c', '2', '-t', '5', gw0], timeout=15)[0] == 0) if back else False
    result.update(originalRoute=bool(back), gatewayPing=bool(ping), confirmed=bool(back and ping))
    return result


def main(argv=None, sy=None, out_dir=None):
    sy = sy or System()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True)
    ap.add_argument('--return-ssid', required=True)
    ap.add_argument('--iface', default='en0')
    ap.add_argument('--budget', type=int, default=900)
    ap.add_argument('--python', default=str(DEFAULT_PYTHON if DEFAULT_PYTHON.exists() else sys.executable))
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args(argv)
    os.umask(0o077)
    stamp = time.strftime('%Y-%m-%d-%H%M%S')
    out_dir = Path(out_dir or ROOT / 'logs/diagnostics'); out_dir.mkdir(parents=True, exist_ok=True)
    report = {'stamp': stamp, 'dryRun': args.dry_run, 'steps': [], 'e1': 'NOT RUN', 'restore': None}
    def log(name, **kw):
        report['steps'].append({'step': name, **kw}); print(name, kw, flush=True)
    def save():
        path = out_dir / f'{stamp}-e1-run.json'
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, 'w') as f: json.dump(report, f, indent=1)

    ap_ssid, ap_pwd = ap_credentials()
    iface0, gw0 = default_route(sy)
    known = preferred(sy, args.iface) or []
    py_ok = sy.run([args.python, '-c', 'import serial'], timeout=20)[0] == 0
    pre_ok = iface0 == args.iface and bool(gw0) and not gw0.startswith(AP_PREFIX) and args.return_ssid in known and py_ok
    log('preflight', defaultIface=iface0, originalGatewayRecorded=bool(gw0), returnSsidPreferred=args.return_ssid in known,
        apKnownBefore=ap_ssid in known, pythonHasPyserial=py_ok, ok=pre_ok)
    if not pre_ok:
        report['e1'] = 'NOT RUN (preflight refused, network untouched)'; save(); return 2
    if args.dry_run:
        log('plan', commands=['join <AP>', 'wait 192.168.4.x 60 s', f'check_p1c_http.py --ap budget {args.budget} s',
                              'restore: rejoin, original-gateway check, power-cycle fallback, forget AP, ping'])
        report['e1'] = 'NOT RUN (dry-run)'; save(); return 0

    def on_signal(signum, frame): raise KeyboardInterrupt(f'signal {signum}')
    for s in (signal.SIGTERM, signal.SIGHUP):
        try: signal.signal(s, on_signal)
        except (ValueError, OSError): pass
    test_ok = False
    try:
        code, out = sy.run(['networksetup', '-setairportnetwork', args.iface, ap_ssid, ap_pwd], timeout=60)
        log('join-ap', code=code, output=out.replace(ap_pwd, '<pw>')[:200])
        addr = wait_for(sy, lambda: (lambda a: a if a and a.startswith(AP_PREFIX) else None)(address(sy, args.iface)), 60)
        log('ap-address', address=addr)
        if not addr:
            report['e1'] = 'NOT RUN (no AP address)'
        else:
            stdout = out_dir / f'{stamp}-e1-http-stdout.txt'; hlog = out_dir / f'{stamp}-e1-http.log'
            rc = sy.run_child([args.python, str(ROOT / 'scripts/check_p1c_http.py'), '--port', args.port, '--ap', '--log', str(hlog)],
                              str(stdout), args.budget, str(ROOT / 'scripts'))
            log('check', exit=rc, stdout=stdout.name, log=hlog.name)
            test_ok = rc == 0
            report['e1'] = 'PASS' if test_ok else f'FAIL ({rc})'
    except BaseException as e:  # includes Ctrl-C/SIGTERM: fall through to restore
        log('interrupted', error=repr(e)); report['e1'] = f'NOT RUN (interrupted: {e!r})'
    finally:
        try:
            report['restore'] = restore(sy, args.iface, args.return_ssid, gw0, ap_ssid, ap_ssid in known, log)
        except BaseException as e:
            report['restore'] = {'confirmed': False, 'error': repr(e)}
        save()
    restored = bool(report['restore'] and report['restore'].get('confirmed'))
    return 0 if test_ok and restored else 5 if not test_ok and not restored else 3 if not test_ok else 4


if __name__ == '__main__':
    sys.exit(main())
