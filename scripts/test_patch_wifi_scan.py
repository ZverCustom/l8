"""Exercise the build script's idempotence and refusal to change an unknown core."""
import runpy
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("patch_wifi_scan.py")


class ScanPatchTest(unittest.TestCase):
    def run_patch(self, root):
        class Env:
            def PioPlatform(self):
                return self

            def get_package_dir(self, name):
                assert name == "framework-arduinoespressif32"
                return str(root)

        runpy.run_path(str(SCRIPT), init_globals={"Import": lambda _: None, "env": Env()})

    def fixture(self, root, source):
        path = root / "libraries/WiFi/src/WiFiScan.cpp"
        path.parent.mkdir(parents=True)
        path.write_text(source)
        return path

    def test_repeat_build_does_not_rewrite_framework(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = self.fixture(root, "void scan() { wifi_scan_config_t config; }\n")
            self.run_patch(root)
            after, stamp = path.read_bytes(), path.stat().st_mtime_ns
            self.run_patch(root)
            self.assertEqual((after, stamp), (path.read_bytes(), path.stat().st_mtime_ns))

    def test_unknown_core_is_left_untouched(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = "void scan() { wifi_scan_config_t config = different_defaults; }\n"
            path = self.fixture(root, source)
            with self.assertRaises(RuntimeError):
                self.run_patch(root)
            self.assertEqual(source, path.read_text())


if __name__ == "__main__":
    unittest.main()
