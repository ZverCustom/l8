#!/usr/bin/env python3
"""USB check of P1-B savedRevision: set_config/preview/discard/save, reboot persistence, restore.
Backs up get_config (password is redacted by firmware; keepPassword keeps it on restore)."""
import argparse, copy, json, time
from pathlib import Path
import serial
from usb_restore import restore
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--port', required=True)
ap.add_argument('--log', type=Path)
ap.add_argument('--backup', type=Path, required=True)
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
    raise TimeoutError(str(data.get('cmd')))
def check(name, value):
    assert value, name
    results.append(name); print('PASS', name, flush=True)
def status(): return cmd({'cmd': 'status'})
def saved(st): return st['savedRevision'] is not None and st['savedRevision'] == st['configRevision']
def reboot():
    assert cmd({'cmd': 'action', 'action': 'reboot'})['ok']
    time.sleep(1); s.close(); time.sleep(9); s.open(); time.sleep(.5)
    st = status()
    while not st['otaConfirmed']: time.sleep(1); st = status()
    return st
original = None
try:
    time.sleep(7)
    info = cmd({'cmd': 'info'}); st = status()
    check('capability config.savedRevision', 'config.savedRevision' in info['capabilities'])
    check('status bootId equals info bootId', st['bootId'] == info['bootId'])
    check('USB-only, Master OFF', st['vbat'] < 1 and not st['master'])
    check('loaded schema 5 is saved', saved(st))
    original = cmd({'cmd': 'get_config'}); args.backup.write_text(json.dumps(original))
    check('password redacted in backup', original['network']['password'] == '' and original['network']['keepPassword'])
    changed = copy.deepcopy(original); changed['strips'][0]['brightness'] = 37 if original['strips'][0]['brightness'] != 37 else 38
    r = cmd({'cmd': 'set_config', 'config': changed})
    check('set_config reply: revision ahead of savedRevision', r['ok'] and r['savedRevision'] != r['revision'])
    st = status(); check('status dirty right after set_config', not saved(st))
    time.sleep(2.2); st = status(); check('saved after delayed write', saved(st))
    rev = st['configRevision']
    p = cmd({'cmd': 'preview', 'client': 'usb-check', 'revision': rev, 'strip': 0, 'patch': {'brightness': 12}})
    check('preview reply dirty', p['ok'] and p['savedRevision'] == rev and p['revision'] != rev)
    time.sleep(2.2); st = status(); check('preview stays dirty after flush window', st['previewActive'] and not saved(st))
    d = cmd({'cmd': 'discard', 'client': 'usb-check', 'revision': st['configRevision']})
    check('discard to saved baseline: new token is saved', d['ok'] and d['revision'] != st['configRevision'] and d['savedRevision'] == d['revision'])
    p = cmd({'cmd': 'preview', 'client': 'usb-check', 'revision': d['revision'], 'strip': 0, 'patch': {'brightness': 13}})
    c = cmd({'cmd': 'save', 'client': 'usb-check', 'revision': p['revision']}, timeout=15)
    check('save reply: revision == savedRevision', c['ok'] and c['savedRevision'] == c['revision'])
    st = status(); check('status after save consistent', saved(st) and not st['previewActive'] and not st['master'])
    boot = st['bootId']; st = reboot()
    check('after reboot: new bootId, saved, Master OFF', st['bootId'] != boot and saved(st) and not st['master'])
    check('saved change persisted', cmd({'cmd': 'get_config'})['strips'][0]['brightness'] == 13)
    ok, why = restore(cmd, original)
    check('restore original (confirmed write and content)', ok)
    st = reboot()
    check('original restored after reboot', cmd({'cmd': 'get_config'}) == original and saved(st) and not st['master'])
    original = None
    print(f'{len(results)} checks passed')
finally:
    if original is not None:
        ok, why = restore(cmd, original)
        print('restore after failure: CONFIRMED' if ok else f'restore after failure NOT confirmed: {why}')
    if args.log: args.log.write_text('\n'.join(log) + '\n')
