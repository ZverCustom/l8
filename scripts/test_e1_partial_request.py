"""Offline tests: reset is decided by bootId of matched v1 replies, not by uptime."""
import json, unittest
from e1_partial_request import read_status, verdict


class FakePort:
    def __init__(self, lines): self.lines = [l.encode() for l in lines]; self.sent = []
    def reset_input_buffer(self): pass
    def write(self, b): self.sent.append(json.loads(b))
    def flush(self): pass
    def readline(self): return self.lines.pop(0) if self.lines else b''


def reply(rid, boot, up): return 'L8 ' + json.dumps({'v': 1, 'id': rid, 'ok': True, 'bootId': boot, 'result': {'uptimeMs': up}}) + '\n'


class Clock:
    def __init__(self): self.t = 0
    def __call__(self): self.t += 0.5; return self.t


class PartialRequest(unittest.TestCase):
    def test_reset_with_larger_uptime_is_still_reset(self):  # review R10 case: 7000 -> reset -> 10000
        self.assertEqual(verdict({'bootId': 'a', 'uptimeMs': 7000}, {'bootId': 'b', 'uptimeMs': 10000})[0], 1)

    def test_reset_with_smaller_uptime(self):
        self.assertEqual(verdict({'bootId': 'a', 'uptimeMs': 60000}, {'bootId': 'b', 'uptimeMs': 3000})[0], 1)

    def test_same_boot_is_no_reset(self):
        self.assertEqual(verdict({'bootId': 'a', 'uptimeMs': 7000}, {'bootId': 'a', 'uptimeMs': 25000})[0], 0)

    def test_missing_reply_is_not_measured(self):
        self.assertIsNone(read_status(FakePort([]), 'x', now=Clock()))
        self.assertEqual(verdict(None, {'bootId': 'a'})[0], 2); self.assertEqual(verdict({'bootId': 'a'}, None)[0], 2)

    def test_foreign_or_stale_reply_is_ignored(self):
        port = FakePort(['Wi-Fi event t=1\n', reply('older-id', 'stale', 1), 'L8 {"configRevision":1}\n', reply('mine', 'fresh', 5)])
        self.assertEqual(read_status(port, 'mine', now=Clock()), {'bootId': 'fresh', 'uptimeMs': 5})
        self.assertIsNone(read_status(FakePort([reply('other', 'x', 1)]), 'mine', now=Clock()))

    def test_partial_lines_are_joined(self):
        r = reply('mine', 'b1', 9); port = FakePort([r[:20], r[20:]])
        self.assertEqual(read_status(port, 'mine', now=Clock())['bootId'], 'b1')


if __name__ == '__main__':
    unittest.main()
