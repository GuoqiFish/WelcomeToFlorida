"""Verify block geometry against synthetic index entries."""

import io
import struct
import unittest

from research.check_asset_blocks import block_sizes, logical, physical, read_table


class AssetBlockTests(unittest.TestCase):
    def setUp(self):
        self.first = 1 | (7 << 26) | (15 << 52)
        self.sentinel = 3 | (1 << 30)
        self.index = struct.pack("<QQ", self.first, self.sentinel)
        self.header = struct.pack("<4sIQ", b"RDS5", 2, 4096 | (123 << 40))

    def test_address_fields_exclude_flags_and_trim(self):
        raw, words = read_table(io.BytesIO(self.header + self.index), 768)
        self.assertEqual(raw, self.index)
        self.assertEqual(words, [self.first, self.sentinel])
        self.assertEqual(physical(self.first), 256)
        self.assertEqual(logical(self.first), 0)
        self.assertEqual(block_sizes(*words), (512, 4096, 15))

    def test_bounds_and_sentinel_must_match(self):
        for stream, size in [
            (self.header + self.index[:-1], 768),
            (self.header + self.index, 769),
            (self.header + struct.pack("<QQ", 0, self.sentinel), 768),
            (self.header + struct.pack("<QQ", self.first, 3 | (2 << 30)), 768),
            (struct.pack("<4sIQ", b"RDS5", 200, 4096), 768),
        ]:
            with self.subTest(size=size, stream=stream), self.assertRaises(ValueError):
                read_table(io.BytesIO(stream), size)

    def test_nonpositive_stored_or_logical_size_is_rejected(self):
        for first, after in [
            (1 | (512 << 52), self.sentinel),
            (self.first, 3),
            (self.sentinel, self.first),
        ]:
            with self.subTest(first=first, after=after), self.assertRaises(ValueError):
                block_sizes(first, after)
