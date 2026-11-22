"""Confirmed restore of an L8 configuration over USB legacy commands.

restore(cmd, original, client, wait) ends our own preview lease (if any), applies
the original with set_config, checks the reply, waits for the delayed write and
compares get_config with the original. It returns (confirmed, reason); callers
must report a failure as "restore NOT confirmed", never as success.
"""
import time

def restore(cmd, original, client='usb-check', wait=2.2, sleep=time.sleep):
    try:
        st = cmd({'cmd': 'status'})
        if st.get('previewActive'):
            if st.get('previewOwner') not in (None, client):
                return False, f"preview owned by {st.get('previewOwner')!r}"
            d = cmd({'cmd': 'discard', 'client': client, 'revision': st['configRevision']})
            if not d.get('ok'):
                return False, f"discard refused: {d.get('error')}"
        r = cmd({'cmd': 'set_config', 'config': original})
        if not r.get('ok'):
            return False, f"set_config refused: {r.get('error')}"
        sleep(wait)
        st = cmd({'cmd': 'status'})
        if st.get('savedRevision') is None or st.get('savedRevision') != st.get('configRevision'):
            return False, 'write not confirmed (savedRevision != configRevision)'
        if cmd({'cmd': 'get_config'}) != original:
            return False, 'config differs from original after restore'
        return True, 'confirmed'
    except Exception as e:  # transport error: unknown device state
        return False, f'error: {e}'
