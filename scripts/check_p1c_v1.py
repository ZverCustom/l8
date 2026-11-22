#!/usr/bin/env python3
"""Board check of P1-C: v1 envelope over USB and HTTP, framing, errors, boot change, SSE v1.
Changes only runtime Master and a preview that is saved and then restored; restore is confirmed."""
import argparse, copy, json, socket, time, urllib.request, urllib.error
from pathlib import Path
import serial
from usb_restore import restore
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--port', required=True)
ap.add_argument('--log', type=Path)
ap.add_argument('--backup', type=Path, required=True)
args = ap.parse_args()
s = serial.Serial(); s.port = args.port; s.baudrate = 115200; s.timeout = .25; s.write_timeout = 10; s.dtr = False; s.rts = False; s.open()
log, results, skipped = [], [], []
def send_raw(data: bytes, replies=1, timeout=12):
    s.write(data); s.flush(); out = []; until = time.monotonic() + timeout
    while len(out) < replies and time.monotonic() < until:
        line = s.readline().decode(errors='replace').strip()
        if line: log.append(line[:2000])
        if line.startswith('L8 {'): out.append(json.loads(line[3:]))
    if len(out) < replies: raise TimeoutError(data[:60])
    return out
def usb(obj): return send_raw((json.dumps(obj, separators=(',', ':')) + '\n').encode())[0]
def legacy(obj): return usb(obj)
def check(name, value):
    assert value, name
    results.append(name); print('PASS', name, flush=True)
def err(r, code): return r.get('ok') is False and r.get('error', {}).get('code') == code
def wait_boot():
    time.sleep(1); s.close(); time.sleep(9); s.open(); time.sleep(.5)
    while True:
        st = legacy({'cmd': 'status'})
        if st['otaConfirmed']: return st
        time.sleep(1)
original = None
try:
    time.sleep(7)
    info = usb({'v': 1, 'id': 'i1', 'cmd': 'info'}); boot = info['bootId']
    check('v1 info envelope', info['ok'] and info['v'] == 1 and info['id'] == 'i1' and info['result']['bootId'] == boot)
    check('capabilities P1-C', {'commands.v1', 'preview.v1', 'status.sse'} <= set(info['result']['capabilities']) and info['result']['limits']['maxCommandBytes'] == 24576)
    st = usb({'v': 1, 'id': 's1', 'cmd': 'status'})
    check('v1 status: envelope revisions match status', st['revision'] == st['result']['configRevision'] and st['savedRevision'] == st['result']['savedRevision'] and st['result']['bootId'] == boot)
    check('USB-only, Master OFF', st['result']['vbat'] < 1 and not st['result']['master'])
    original = legacy({'cmd': 'get_config'}); args.backup.write_text(json.dumps(original))
    g = usb({'v': 1, 'id': 'g1', 'cmd': 'get_config'})
    check('v1 get_config baseline equals legacy get_config', g['ok'] and g['result']['view'] == 'baseline' and g['result']['config'] == original)
    # errors
    check('v=2 -> UNSUPPORTED_VERSION', err(usb({'v': 2, 'id': 'e', 'cmd': 'info'}), 'UNSUPPORTED_VERSION'))
    r = send_raw(b'{v:1,id:"e",cmd:"info"}\n')[0]; check('unquoted keys -> BAD_JSON id null', err(r, 'BAD_JSON') and r['id'] is None)
    r = send_raw(b'{"v":1,"id":"a\\u0001","cmd":"info"}\n')[0]; check('control char id -> INVALID_ARGUMENT', err(r, 'INVALID_ARGUMENT') and r['id'] is None)
    check('unknown cmd -> UNKNOWN_COMMAND', err(usb({'v': 1, 'id': 'e', 'cmd': 'nope'}), 'UNKNOWN_COMMAND'))
    r = usb({'v': 1, 'id': 'e', 'cmd': 'action', 'bootId': boot, 'action': 'master', 'on': 'true'})
    check('string boolean refused, Master unchanged', err(r, 'INVALID_ARGUMENT') and not legacy({'cmd': 'status'})['master'])
    # R2-R4: NUL, duplicate keys, raw CR in body, invalid UTF-8, CRLF still fine
    r = send_raw(('{"v":1,"id":"x","cmd":"action\\u0000junk","bootId":"%s","action":"master","on":true}\n' % boot).encode())[0]
    check('NUL in cmd -> INVALID_ARGUMENT, Master off', err(r, 'INVALID_ARGUMENT') and not legacy({'cmd': 'status'})['master'])
    r = send_raw(('{"v":1,"id":"x","cmd":"action","bootId":"%s","action":"master","on":false,"on":true}\n' % boot).encode())[0]
    check('duplicate key -> INVALID_ARGUMENT, Master off', err(r, 'INVALID_ARGUMENT') and not legacy({'cmd': 'status'})['master'])
    r = send_raw(('{"v":1,"id":"x","cmd":"act\rion","bootId":"%s","action":"master","on":true}\n' % boot).encode())[0]
    check('raw CR inside body -> BAD_JSON, Master off', err(r, 'BAD_JSON') and not legacy({'cmd': 'status'})['master'])
    bad_cfg = json.dumps(original, separators=(',', ':')).encode().replace(b'"ssid":"', b'"ssid":"\xff', 1)
    rev0 = usb({'v': 1, 'id': 'r', 'cmd': 'status'})['revision']
    r = send_raw(b'{"v":1,"id":"u","cmd":"apply","bootId":"' + boot.encode() + b'","client":"usb-check","revision":' + str(rev0).encode() + b',"config":' + bad_cfg + b'}\n')[0]
    st = legacy({'cmd': 'status'})
    check('0xFF byte in config -> BAD_JSON, no preview, revision unchanged', err(r, 'BAD_JSON') and not st['previewActive'] and st['configRevision'] == rev0)
    r = send_raw(b'{"v":1,"id":"crlf","cmd":"info"}\r\n')[0]; check('CRLF command accepted', r['ok'] and r['id'] == 'crlf')
    # framing: oversize line, then a valid command in the same write
    big = b'{"v":1,"id":"big","cmd":"status","pad":"' + b'x' * 24600 + b'"}\n'
    replies = send_raw(big + b'{"v":1,"id":"after","cmd":"info"}\n', replies=2, timeout=20)
    check('oversize line -> FRAME_TOO_LARGE, next command runs', err(replies[0], 'FRAME_TOO_LARGE') and replies[0]['id'] is None and replies[1]['ok'] and replies[1]['id'] == 'after')
    pad = 24001 - len('{"cmd":"status","pad":""}')
    r = send_raw(b'{"cmd":"status","pad":"' + b'y' * pad + b'"}\n', timeout=20)[0]
    check('legacy 24001-byte line -> line too long', r.get('error') == 'line too long')
    exact = b'{"v":1,"id":"ok","cmd":"status","pad":"' + b'z' * (24576 - len('{"v":1,"id":"ok","cmd":"status","pad":""}')) + b'"}\n'
    check('v1 24576-byte line accepted', send_raw(exact, timeout=20)[0]['ok'])
    # editor flow
    rev = usb({'v': 1, 'id': 'r', 'cmd': 'status'})['revision']
    p = usb({'v': 1, 'id': 'p', 'cmd': 'preview', 'bootId': boot, 'client': 'usb-check', 'revision': rev, 'strip': 0, 'patch': {'brightness': 21}})
    check('preview result has owner/lease', p['ok'] and p['result']['previewOwner'] == 'usb-check' and p['result']['previewRemainingMs'] > 50000 and p['savedRevision'] != p['revision'])
    e = usb({'v': 1, 'id': 'e', 'cmd': 'get_config', 'view': 'effective', 'client': 'usb-check'})
    check('effective view shows preview', e['ok'] and e['result']['config']['strips'][0]['brightness'] == 21)
    check('effective view refused to other client', err(usb({'v': 1, 'id': 'e', 'cmd': 'get_config', 'view': 'effective', 'client': 'other'}), 'EDITOR_BUSY'))
    check('other editor -> EDITOR_BUSY', err(usb({'v': 1, 'id': 'x', 'cmd': 'preview', 'bootId': boot, 'client': 'other', 'revision': p['revision'], 'strip': 0, 'patch': {'brightness': 5}}), 'EDITOR_BUSY'))
    sv = usb({'v': 1, 'id': 'sv', 'cmd': 'save', 'bootId': boot, 'client': 'usb-check', 'revision': p['revision']})
    check('save: revision == savedRevision', sv['ok'] and sv['revision'] == sv['savedRevision'] and not sv['result']['previewActive'])
    check('save without preview -> PREVIEW_EXPIRED', err(usb({'v': 1, 'id': 'sv', 'cmd': 'save', 'bootId': boot, 'client': 'usb-check', 'revision': sv['revision']}), 'PREVIEW_EXPIRED'))
    # Master gates
    m = usb({'v': 1, 'id': 'm', 'cmd': 'action', 'bootId': boot, 'action': 'master', 'on': True})
    check('Master ON accepted (no 12 V: outputs stay off)', m['ok'] and m['result']['master'] and m['revision'] == sv['revision'])
    check('reset with Master ON -> MASTER_MUST_BE_OFF', err(usb({'v': 1, 'id': 'x', 'cmd': 'action', 'bootId': boot, 'action': 'reset', 'channel': -1}), 'MASTER_MUST_BE_OFF'))
    check('reboot with Master ON -> MASTER_MUST_BE_OFF', err(usb({'v': 1, 'id': 'x', 'cmd': 'action', 'bootId': boot, 'action': 'reboot'}), 'MASTER_MUST_BE_OFF'))
    stt = legacy({'cmd': 'status'}); check('outputs/DATA off with Master ON on USB', all(not c['on'] and not c['data'] for c in stt['channels']))
    check('Master OFF', usb({'v': 1, 'id': 'm', 'cmd': 'action', 'bootId': boot, 'action': 'master', 'on': False})['ok'])
    # HTTP parity, auth and SSE
    st = legacy({'cmd': 'status'})
    for attempt in range(4):  # flaky home AP: ask for an explicit reconnect instead of rebooting
        until = time.monotonic() + 45
        while st.get('wifi', {}).get('phase') != 'online' and time.monotonic() < until: time.sleep(1); st = legacy({'cmd': 'status'})
        if st.get('wifi', {}).get('phase') == 'online': break
        print('Wi-Fi not online, reconnect', attempt + 1, flush=True); legacy({'cmd': 'action', 'action': 'reconnect'}); st = legacy({'cmd': 'status'})
    if st.get('wifi', {}).get('phase') == 'online':
        base = f"http://{st['ip']}"
        # Lossy link (ping loss seen): retry only requests that are reads or refused by validation.
        def retry(fn):
            for attempt in range(12):
                try: return fn()
                except (urllib.error.URLError, TimeoutError, ConnectionError, socket.timeout) as e:
                    if isinstance(e, urllib.error.HTTPError): raise
                    print('  network retry', attempt + 1, type(e).__name__, flush=True); time.sleep(2)
            raise TimeoutError('HTTP unreachable after retries')
        token = retry(lambda: json.loads(urllib.request.urlopen(base + '/api/session', timeout=15).read()))['token']
        def post(obj, tok=token):
            def once():
                req = urllib.request.Request(base + '/api/command', data=json.dumps(obj).encode(), headers={'Content-Type': 'application/json', **({'X-L8-Token': tok} if tok else {})})
                try: resp = urllib.request.urlopen(req, timeout=15); return resp.status, json.loads(resp.read())
                except urllib.error.HTTPError as h: return h.code, json.loads(h.read())
            return retry(once)
        code, hi = post({'v': 1, 'id': 'i1', 'cmd': 'info'})
        check('HTTP info == USB info (same boot)', code == 200 and hi == usb({'v': 1, 'id': 'i1', 'cmd': 'info'}))
        code, hg = post({'v': 1, 'id': 'g1', 'cmd': 'get_config'})
        check('HTTP get_config == USB get_config', code == 200 and hg == usb({'v': 1, 'id': 'g1', 'cmd': 'get_config'}))
        bad = {'v': 1, 'id': 'b', 'cmd': 'action', 'bootId': boot, 'action': 'reset', 'channel': 9}
        code, hb = post(bad); check('HTTP/USB same error, HTTP 400', code == 400 and hb == usb(bad))
        code, ha = post({'v': 1, 'id': 'a', 'cmd': 'status'}, tok=None); check('HTTP without token -> 403 AUTH_REQUIRED', code == 403 and err(ha, 'AUTH_REQUIRED') and ha['id'] == 'a')
        code, hv = post({'cmd': 'status'}); check('HTTP /api/command without v -> 400', code == 400 and err(hv, 'UNSUPPORTED_VERSION'))
        host = st['ip']
        def sse(query):
            def once():
                c = socket.create_connection((host, 80), timeout=15); c.sendall(f'GET /api/events?{query} HTTP/1.1\r\nHost: {host}\r\n\r\n'.encode()); return c
            return retry(once)
        c = sse(f'v=1&token={token}'); buf = b''; until = time.monotonic() + 5
        def complete(b): return b'data: ' in b and b'\n\n' in b.split(b'data: ', 1)[1]
        while not complete(buf) and time.monotonic() < until: buf += c.recv(65536)
        c.close(); payload = json.loads(buf.split(b'data: ', 1)[1].split(b'\n\n', 1)[0])
        check('SSE v1 envelope coherent', payload['v'] == 1 and payload['bootId'] == boot and payload['revision'] == payload['status']['configRevision'] and payload['savedRevision'] == payload['status']['savedRevision'] and isinstance(payload['seq'], int))
        c = sse(f'v=2&token={token}'); head = c.recv(4096); c.close(); check('SSE v=2 -> 400', b' 400 ' in head.split(b'\r\n', 1)[0])
        time.sleep(1); conns = [sse(f'v=1&token={token}') for _ in range(3)]; time.sleep(1.5)
        c4 = sse(f'token={token}'); head = c4.recv(4096); c4.close()
        for c in conns: c.close()
        check('4th SSE client refused (503), limit shared by v1/legacy', b' 503 ' in head.split(b'\r\n', 1)[0])
    else:
        skipped.append('HTTP/SSE (Wi-Fi not online)'); print('SKIP HTTP/SSE: Wi-Fi not online', flush=True)
    # boot change
    usb({'v': 1, 'id': 'rb', 'cmd': 'action', 'bootId': boot, 'action': 'reboot'}); wait_boot()
    info2 = usb({'v': 1, 'id': 'i2', 'cmd': 'info'})
    r = usb({'v': 1, 'id': 'old', 'cmd': 'action', 'bootId': boot, 'action': 'master', 'on': True})
    check('old bootId after reboot -> BOOT_CHANGED, Master off', info2['result']['bootId'] != boot and err(r, 'BOOT_CHANGED') and r['bootId'] == info2['result']['bootId'] and not legacy({'cmd': 'status'})['master'])
    ok, why = restore(legacy, original); check('restore original (confirmed)', ok)
    original = None
    print(f'{len(results)} checks passed; skipped: {skipped or "none"}')
finally:
    if original is not None:
        ok, why = restore(legacy, original)
        print('restore after failure: CONFIRMED' if ok else f'restore after failure NOT confirmed: {why}')
    if args.log: args.log.write_text('\n'.join(log) + '\n')
