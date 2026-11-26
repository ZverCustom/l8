"""Offline tests of e1_images.py (disassembly check, manifest verification, flash arguments)."""
import json, tempfile, unittest
from pathlib import Path

import e1_images

# Excerpts of objdump output of the two pinned ELFs (2026-11-26).
OLD = """4201f44c <_ZN6Stream9timedReadEv>:
4201f44c:\tentry\ta1, 32
4201f44f:\tcall8\t42021054 <millis>
4201f45d:\tcallx8\ta8
4201f463:\tcall8\t42021054 <millis>
4201f46d:\tbltu\ta10, a8, 4201f454 <_ZN6Stream9timedReadEv+0x8>
4201f474:\tretw.n
\t...

4201f478 <_ZN6Stream9timedPeekEv>:
4201f480:\tcall8\t42021074 <delay>
"""
NEW = """4201f44c <_ZN6Stream9timedReadEv>:
4201f44c:\tentry\ta1, 32
4201f45c:\tcallx8\ta8
4201f462:\tmov.n\ta10, a3
4201f464:\tcall8\t42021074 <delay>
4201f467:\tcall8\t42021058 <millis>
4201f478:\tretw.n
\t...

"""


class Images(unittest.TestCase):
    def test_disassembly(self):
        self.assertIs(e1_images.timed_read_calls_delay(OLD), False, 'delay in the next function must not count')
        self.assertIs(e1_images.timed_read_calls_delay(NEW), True)
        self.assertIsNone(e1_images.timed_read_calls_delay('42000000 <other>:\n'))

    def make(self, role, calls_delay):
        d = Path(tempfile.mkdtemp())
        files = {}
        for n in e1_images.FILES:
            (d / n).write_bytes(n.encode()); files[n] = {'size': (d / n).stat().st_size, 'sha256': e1_images.sha256(d / n)}
        (d / 'manifest.json').write_text(json.dumps({'role': role, 'sourceCommit': 'abc', 'files': files, 'streamTimedReadCallsDelay': calls_delay,
                                                     'flash': {'mode': 'dio', 'freq': '80m', 'size': '16MB', 'baud': 460800}}))
        return d / 'manifest.json'

    def test_verify(self):
        self.assertTrue(e1_images.verify(self.make('old', False))[0])
        self.assertTrue(e1_images.verify(self.make('new', True))[0])
        self.assertFalse(e1_images.verify(self.make('old', True))[0])
        self.assertFalse(e1_images.verify(self.make('new', None))[0])
        m = self.make('old', False); (m.parent / 'firmware.bin').write_bytes(b'x')
        ok, why, _ = e1_images.verify(m); self.assertFalse(ok); self.assertIn('firmware.bin hash/size mismatch', why)
        m = self.make('old', False); (m.parent / 'boot_app0.bin').unlink()
        self.assertFalse(e1_images.verify(m)[0])
        self.assertFalse(e1_images.verify('/nonexistent/manifest.json')[0])

    def test_flash_args_match_platformio_upload(self):
        m = self.make('new', True); man = json.loads(m.read_text())
        a = e1_images.flash_args(man, m.parent)
        self.assertEqual(a[:15], ['--chip', 'esp32s3', '--baud', '460800', '--before', 'default_reset', '--after', 'hard_reset',
                                  'write_flash', '-z', '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size'])
        pairs = list(zip(a[16::2], [Path(x).name for x in a[17::2]]))
        self.assertEqual(pairs, [('0x0', 'bootloader.bin'), ('0x8000', 'partitions.bin'), ('0xe000', 'boot_app0.bin'), ('0x10000', 'firmware.bin')])


if __name__ == '__main__':
    unittest.main()
