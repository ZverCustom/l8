"""Stream yield patch: applies both loops once, is idempotent, refuses unknown cores."""
import runpy, tempfile, unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("patch_stream_yield.py")
ORIGINAL = """int Stream::timedRead()
{
    int c;
    _startMillis = millis();
    do {
        c = read();
        if(c >= 0) {
            return c;
        }
    } while(millis() - _startMillis < _timeout);
    return -1;     // -1 indicates timeout
}
int Stream::timedPeek()
{
    int c;
    _startMillis = millis();
    do {
        c = peek();
        if(c >= 0) {
            return c;
        }
    } while(millis() - _startMillis < _timeout);
    return -1;     // -1 indicates timeout
}
"""


class StreamPatch(unittest.TestCase):
    def run_patch(self, root):
        class Env:
            def PioPlatform(self): return self
            def get_package_dir(self, name):
                assert name == "framework-arduinoespressif32"; return str(root)
        runpy.run_path(str(SCRIPT), init_globals={"Import": lambda _: None, "env": Env()})

    def fixture(self, root, text):
        p = root / "cores/esp32/Stream.cpp"; p.parent.mkdir(parents=True); p.write_text(text); return p

    def test_patches_both_loops_once_and_is_idempotent(self):
        with tempfile.TemporaryDirectory() as d:
            p = self.fixture(Path(d), ORIGINAL); self.run_patch(Path(d))
            text = p.read_text(); self.assertEqual(text.count("delay(1); // L8"), 2)
            self.assertLess(text.index("return c;"), text.index("delay(1)"))
            stamp = p.stat().st_mtime_ns; self.run_patch(Path(d))
            self.assertEqual((text, stamp), (p.read_text(), p.stat().st_mtime_ns))

    def test_unknown_core_untouched(self):
        with tempfile.TemporaryDirectory() as d:
            changed = ORIGINAL.replace("c = peek();", "c = this->peek();")
            p = self.fixture(Path(d), changed)
            with self.assertRaises(RuntimeError): self.run_patch(Path(d))
            self.assertEqual(p.read_text(), changed)


if __name__ == "__main__":
    unittest.main()
