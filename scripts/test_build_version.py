#!/usr/bin/env python3
"""Exercise the actual PlatformIO pre-script without a toolchain or repository."""
import json
from contextlib import nullcontext
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).with_name('build_version.py')

class VersionTest(unittest.TestCase):
    def generate(self, responses):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            root.joinpath('VERSION').write_text('0.4.0\n')
            class Env:
                def subst(self, key):
                    return str(root if key == '$PROJECT_DIR' else root / 'build')
                def Append(self, **kwargs):
                    pass
            with (patch('version_info.subprocess.check_output', side_effect=responses(root)) if responses else nullcontext()):
                runpy.run_path(str(SCRIPT), init_globals={'env': Env(), 'Import': lambda _: None})
            header = root.joinpath('build/generated/l8_version.h').read_text()
            return dict((line.split()[1], json.loads(line.split(' ', 2)[2]))
                        for line in header.splitlines() if line.startswith('#define'))

    def test_clean_and_dirty_checkout(self):
        for revision in ['abc1234', 'v0.4-2-gabc1234-dirty']:
            got = self.generate(lambda root: [str(root), revision])
            self.assertEqual(got['L8_FW_VERSION'], '0.4.0+' + revision)
            self.assertEqual(got['GIT_REV'], revision)

    def test_real_archive_probe(self):
        self.assertEqual(self.generate(None)['L8_FW_VERSION'], '0.4.0+unknown')

    def test_missing_git_executable(self):
        self.assertEqual(self.generate(lambda _: FileNotFoundError())['L8_FW_VERSION'], '0.4.0+unknown')

    def test_archive_or_unavailable_git_metadata(self):
        self.assertEqual(self.generate(lambda _: subprocess.CalledProcessError(128, 'git'))['GIT_REV'], 'unknown')

    def test_archive_inside_unrelated_checkout(self):
        self.assertEqual(self.generate(lambda root: [str(root.parent)])['L8_FW_VERSION'], '0.4.0+unknown')

if __name__ == '__main__':
    unittest.main()
