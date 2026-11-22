#!/usr/bin/env python3
"""Bounded, unattended E1 run over the board's AP with guaranteed network return.

Runs entirely locally (start it detached, e.g. `nohup ... &`), so it does not depend on
an agent or a chat while the computer has no internet:

  1. preflight: Wi-Fi interface, default route, return network known (--return-ssid,
     needed because macOS redacts the current SSID without Location Services);
  2. join the board AP (SSID/password from src/config/config.h, never printed);
  3. wait for a 192.168.4.x address, record it;
  4. run scripts/check_p1c_http.py --ap with a hard time budget;
  5. ALWAYS (success, failure, timeout, Ctrl-C): rejoin the return network, fall back to
     a Wi-Fi power cycle (macOS auto-join), forget ZVER-L8 if it was not known before,
     and confirm the default route and gateway before exiting.

--dry-run prints the plan and preflight results without changing any network setting.
Results go to logs/diagnostics/<stamp>-e1-run.json (private, ignored by Git)."""
import argparse, json, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def sh(*cmd, timeout=30):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return r.returncode, (r.stdout + r.stderr).strip()


def ap_credentials():
    text = (ROOT / 'src/config/config.h').read_text()
    ssid = re.search(r'#define\s+AP_SSID\s+"([^"]*)"', text).group(1)
    pwd = re.search(r'#define\s+AP_PASSWORD\s+"([^"]*)"', text).group(1)
    return ssid, pwd


def default_route():
    code, out = sh('route', '-n', 'get', 'default')
    iface = re.search(r'interface:\s*(\S+)', out); gw = re.search(r'gateway:\s*(\S+)', out)
    return (iface.group(1) if iface else None), (gw.group(1) if gw else None)


def preferred(iface):
    code, out = sh('networksetup', '-listpreferredwirelessnetworks', iface)
    return [l.strip() for l in out.splitlines()[1:]]


def wait_for(pred, seconds, step=2):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        v = pred()
        if v: return v
        time.sleep(step)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True)
    ap.add_argument('--return-ssid', required=True, help='network to rejoin afterwards (macOS hides the current one)')
    ap.add_argument('--iface', default='en0')
    ap.add_argument('--budget', type=int, default=900, help='hard limit for the HTTP/SSE check, seconds')
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()
    stamp = time.strftime('%Y-%m-%d-%H%M%S')
    out_dir = ROOT / 'logs/diagnostics'; out_dir.mkdir(parents=True, exist_ok=True)
    report = {'stamp': stamp, 'dryRun': args.dry_run, 'steps': []}
    def step(name, **kw):
        report['steps'].append({'step': name, 't': round(time.time(), 1), **kw}); print(name, kw, flush=True)

    ap_ssid, ap_pwd = ap_credentials()
    iface0, gw0 = default_route()
    known = preferred(args.iface)
    ok_pre = iface0 == args.iface and gw0 is not None and args.return_ssid in known
    step('preflight', defaultIface=iface0, gatewayPresent=gw0 is not None, returnSsidInPreferredList=args.return_ssid in known,
         apKnownBefore=ap_ssid in known, ok=ok_pre)
    if not ok_pre:
        step('abort', reason='return network not verifiable; network not switched'); finish(report, out_dir, stamp); return 2
    plan = [f'networksetup -setairportnetwork {args.iface} <AP_SSID> <AP_PASSWORD>',
            'wait for 192.168.4.x (60 s)',
            f'check_p1c_http.py --port {args.port} --ap (budget {args.budget} s)',
            f'networksetup -setairportnetwork {args.iface} <return-ssid>; fallback: Wi-Fi power off/on (auto-join)',
            'forget AP if it was not known before', 'confirm default route + gateway ping (90 s)']
    if args.dry_run:
        step('plan', commands=plan); finish(report, out_dir, stamp); return 0
    try:
        code, out = sh('networksetup', '-setairportnetwork', args.iface, ap_ssid, ap_pwd, timeout=60)
        step('join-ap', code=code, output=out.replace(ap_pwd, '<pw>')[:200])
        addr = wait_for(lambda: (lambda c_o: c_o[1] if c_o[0] == 0 and c_o[1].startswith('192.168.4.') else None)(sh('ipconfig', 'getifaddr', args.iface)), 60)
        step('ap-address', address=addr)
        if addr:
            log = out_dir / f'{stamp}-e1-http.log'; stdout = out_dir / f'{stamp}-e1-http-stdout.txt'
            try:
                with open(stdout, 'w') as f:
                    r = subprocess.run([sys.executable, str(ROOT / 'scripts/check_p1c_http.py'), '--port', args.port, '--ap', '--log', str(log)],
                                       stdout=f, stderr=subprocess.STDOUT, timeout=args.budget, cwd=ROOT / 'scripts')
                step('check', exit=r.returncode, stdout=str(stdout.name), log=str(log.name))
            except subprocess.TimeoutExpired:
                step('check', exit='timeout', budget=args.budget)
    finally:
        code, out = sh('networksetup', '-setairportnetwork', args.iface, args.return_ssid, timeout=60)
        step('rejoin', code=code, output=out[:200])
        back = wait_for(lambda: default_route()[0] == args.iface and default_route()[1], 45)
        if not back:
            sh('networksetup', '-setairportpower', args.iface, 'off'); time.sleep(3); sh('networksetup', '-setairportpower', args.iface, 'on')
            step('power-cycle-fallback')
            back = wait_for(lambda: default_route()[0] == args.iface and default_route()[1], 90)
        if ap_ssid not in known:
            sh('networksetup', '-removepreferredwirelessnetwork', args.iface, ap_ssid)
        gw = default_route()[1]
        ping = sh('ping', '-c', '2', '-t', '5', gw)[0] == 0 if gw else False
        step('restored', defaultRoute=bool(back), gatewayPing=ping, apForgotten=ap_ssid not in known)
        finish(report, out_dir, stamp)
    return 0


def finish(report, out_dir, stamp):
    (out_dir / f'{stamp}-e1-run.json').write_text(json.dumps(report, indent=1))


if __name__ == '__main__':
    sys.exit(main())
