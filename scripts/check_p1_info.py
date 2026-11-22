#!/usr/bin/env python3
"""USB check of P1-A: info fields, stable deviceId, new bootId after reboot, reason on USB power."""
import argparse, json, re, time, urllib.request
from pathlib import Path
import serial
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--port', required=True)
ap.add_argument('--log', type=Path)
args = ap.parse_args()
s = serial.Serial(); s.port = args.port; s.baudrate = 115200; s.timeout = .25; s.dtr = False; s.rts = False; s.open()
log, results = [], []
def cmd(data, timeout=12):
    s.write((json.dumps(data, separators=(',', ':')) + '\n').encode()); s.flush()
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        line = s.readline().decode(errors='replace').strip()
        if line: log.append(line)
        if line.startswith('L8 {'): return json.loads(line[3:])
    raise TimeoutError(str(data))
def check(name, value):
    assert value, name
    results.append(name); print('PASS', name, flush=True)
REASONS = {'on', 'io', 'fault', 'inhibit', 'novbat', 'ign', 'master', 'disabled', 'engine', 'retry'}
try:
    time.sleep(7)
    a = cmd({'cmd': 'info'}); b = cmd({'cmd': 'info'})
    check('info product/hardware/schema', a['product'] == 'ZVER L8' and a['hardware'] == 3 and a['configSchema'] == 5)
    check('deviceId format', re.fullmatch(r'L8-[0-9A-F]{12}', a['deviceId']) is not None)
    check('bootId 32 hex', re.fullmatch(r'[0-9a-f]{32}', a['bootId']) is not None and a['bootId'] != '0' * 32)
    check('ids stable within boot', a['deviceId'] == b['deviceId'] and a['bootId'] == b['bootId'])
    check('protocol 1.0', a['protocol'] == {'major': 1, 'minor': 0})
    check('capabilities only implemented', set(a['capabilities']) == {'info.v1', 'channel.reason', 'ota.http'})
    l = a['limits']
    check('limits', l['channels'] == 8 and l['maxLedCount'] == 1000 and l['maxChannelAmps'] == 5 and l['previewLeaseMs'] == 60000
          and l['maxConfigBytes'] == 24000 and l['maxSseClients'] == 3 and l['statusIntervalMs'] == 500)
    check('firmware matches status', a['firmware'] == cmd({'cmd': 'status'})['firmware'])
    st = cmd({'cmd': 'status'})
    check('USB-only precondition', st['vbat'] < 1 and not st['master'])
    check('every channel has reason', all(c.get('reason') in REASONS for c in st['channels']))
    check('USB power -> novbat on all channels', all(c['reason'] == 'novbat' and not c['on'] for c in st['channels']))
    http = None; until = time.monotonic() + 30
    while st.get('wifi', {}).get('phase') != 'online' and time.monotonic() < until:
        time.sleep(1); st = cmd({'cmd': 'status'})
    ip = st.get('ip')
    if st.get('wifi', {}).get('phase') == 'online' and ip:
        http = json.loads(urllib.request.urlopen(f'http://{ip}/api/info', timeout=5).read())
        check('HTTP /api/info equals USB info', http == a)
    r = cmd({'cmd': 'action', 'action': 'master', 'on': True})
    check('Master ON accepted', r['ok'] and r['master'])
    time.sleep(.3); st = cmd({'cmd': 'status'})
    check('novbat outranks master while Master ON', all(c['reason'] == 'novbat' and not c['on'] and not c['data'] for c in st['channels']))
    check('Master OFF', cmd({'cmd': 'action', 'action': 'master', 'on': False})['ok'])
    check('reboot accepted', cmd({'cmd': 'action', 'action': 'reboot'})['ok'])
    time.sleep(1); s.close(); time.sleep(9); s.open()
    c = cmd({'cmd': 'info'}, timeout=20)
    check('deviceId survives reboot', c['deviceId'] == a['deviceId'])
    check('bootId changes on reboot', c['bootId'] != a['bootId'])
    st = cmd({'cmd': 'status'})
    if not st['otaConfirmed']:
        check('before self-check: inhibit outranks novbat', all(x['reason'] == 'inhibit' for x in st['channels']))
        time.sleep(max(0, 6.5 - st['uptimeMs'] / 1000)); st = cmd({'cmd': 'status'})
    check('after self-check: novbat', all(x['reason'] == 'novbat' for x in st['channels']))
    check('safe after reboot', not st['master'] and all(not x['on'] and not x['data'] for x in st['channels']) and st['otaConfirmed'])
    print(f'{len(results)} checks passed; http={"yes" if http else "skipped (not online)"}')
finally:
    if args.log: args.log.write_text('\n'.join(log) + '\n')
