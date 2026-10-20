"""Format/order tests for the offline RPCS3 importer (no copyrighted fixtures)."""
import gzip
from pathlib import Path
import struct
import tempfile
import unittest

from rsx_capture import read_capture, methods, export, write_capture, complete_display_flip


def p(fmt, *args):
    return struct.pack('<' + fmt, *args)


def fixture(memory_id=2):
    data = p('III', 0x435252, 6, 1)
    data += b'\x01' + bytes(432) + p('Q', 1)
    data += b'\x01' + p('IIQQ', 16, 1, 3, 2)
    data += b'\x01\x04' + b'abcd' + p('Q', 3)
    data += b'\x01' + p('33I', *([1280, 720, 5120, 65536] + [0]*28 + [1])) + p('Q', 4)
    commands = [(0, 0, [memory_id], 1, 4),
                ((3 << 18) | 0xb80, 11, [], 0, 0),
                (0, 12, [], 0, 0), (0, 13, [], 0, 0),
                (0x40000000 | (2 << 18) | 0x1f00, 21, [], 0, 0),
                (0, 22, [], 0, 0)]
    data += bytes([len(commands)])
    for word, value, ids, tile, display in commands:
        data += p('II', word, value) + bytes([len(ids)])
        data += p('Q'*len(ids), *ids) + p('QQ', tile, display)
    return data + bytes(544*4*4 + 0x10000)


class CaptureTests(unittest.TestCase):
    def test_memory_order_packets_and_export(self):
        with tempfile.TemporaryDirectory() as d:
            source = Path(d)/'frame.rrc.gz'
            source.write_bytes(gzip.compress(fixture()))
            cap = read_capture(source)
            commands = list(methods(cap))
            self.assertEqual([c[1] for c in commands],
                             [None, 0xb80, 0xb84, 0xb88, 0x1f00, 0x1f00])
            self.assertEqual(commands[0][3:], ((2,), 1, 4))
            self.assertEqual(cap['memory'][2], (16, 1, 3))
            self.assertEqual(cap['blobs'][3], b'abcd')
            output = Path(d)/'frame.msrx'
            export(cap, output)
            self.assertTrue(output.read_bytes().startswith(b'MSRX0001'))

    def test_missing_memory_state_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            source = Path(d)/'frame.rrc'
            source.write_bytes(fixture(memory_id=99))
            with self.assertRaisesRegex(ValueError, 'Missing referenced state'):
                read_capture(source)

    def test_roundtrip_and_final_display_handoff(self):
        with tempfile.TemporaryDirectory() as d:
            source = Path(d)/'frame.rrc'
            source.write_bytes(fixture())
            cap = read_capture(source)
            with self.assertRaises(ValueError):
                complete_display_flip(cap)
            cap['commands'].append(((1 << 18) | 0xe944, 0, (), 0, 0))
            completed = complete_display_flip(cap)
            self.assertEqual(completed['commands'][:-1], cap['commands'])
            self.assertEqual(list(methods(completed))[-1][1:3], (0xe924, 0x80000100))
            output = Path(d)/'copy.rrc.gz'
            write_capture(completed, output)
            restored = read_capture(output)
            for key in ('commands','vp','regs','memory','blobs','tiles','displays'):
                self.assertEqual(restored[key], completed[key])

    def test_truncation_and_trailing_bytes_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            source = Path(d)/'frame.rrc'
            for data in (fixture()[:-1], fixture()+b'X'):
                source.write_bytes(data)
                with self.assertRaises(ValueError):
                    read_capture(source)


if __name__ == '__main__':
    unittest.main()
