"""Restore helper must never report success it did not confirm."""
import unittest
from usb_restore import restore

ORIGINAL = {'schema': 5, 'x': 1}

class Device:
    def __init__(self, preview_owner=None, refuse_set=False, refuse_discard=False, lose_write=False, alter=False):
        self.cfg = {'schema': 5, 'x': 2}; self.rev = 7; self.saved = 7
        self.preview_owner = preview_owner; self.refuse_set = refuse_set; self.refuse_discard = refuse_discard
        self.lose_write = lose_write; self.alter = alter
    def __call__(self, c):
        k = c['cmd']
        if k == 'status':
            return {'previewActive': self.preview_owner is not None, 'previewOwner': self.preview_owner, 'configRevision': self.rev, 'savedRevision': self.saved}
        if k == 'discard':
            if self.refuse_discard: return {'ok': False, 'error': 'busy'}
            self.preview_owner = None; self.rev += 1; return {'ok': True}
        if k == 'set_config':
            if self.refuse_set or self.preview_owner: return {'ok': False, 'error': 'OTA or preview in progress'}
            self.cfg = dict(c['config']); self.rev += 2
            if self.alter: self.cfg['x'] = 99
            if not self.lose_write: self.saved = self.rev
            return {'ok': True}
        if k == 'get_config': return dict(self.cfg)
        raise AssertionError(k)

def run(dev): return restore(dev, ORIGINAL, sleep=lambda s: None)

class RestoreTest(unittest.TestCase):
    def test_success_is_confirmed(self): self.assertEqual(run(Device()), (True, 'confirmed'))
    def test_own_preview_is_discarded_first(self): self.assertTrue(run(Device(preview_owner='usb-check'))[0])
    def test_foreign_preview_is_not_confirmed(self): self.assertFalse(run(Device(preview_owner='web'))[0])
    def test_refused_discard(self): self.assertFalse(run(Device(preview_owner='usb-check', refuse_discard=True))[0])
    def test_refused_set_config(self): self.assertFalse(run(Device(refuse_set=True))[0])
    def test_unconfirmed_write(self): self.assertFalse(run(Device(lose_write=True))[0])
    def test_content_mismatch(self): self.assertFalse(run(Device(alter=True))[0])
    def test_transport_error(self):
        def broken(c): raise TimeoutError('status')
        self.assertFalse(restore(broken, ORIGINAL, sleep=lambda s: None)[0])

if __name__ == '__main__': unittest.main()
