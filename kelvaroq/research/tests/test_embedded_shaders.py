"""Count synthetic DXIL program ranges while ignoring incidental signatures."""

import struct
import unittest

from research.check_embedded_shaders import dxbc_parts, inspect_program, scan


def part(tag, payload):
    return struct.pack("<4sI", tag, len(payload)) + payload


def container(*parts):
    offsets = []
    length = 32 + 4 * len(parts)
    for entry in parts:
        offsets.append(length)
        length += len(entry)
    header = struct.pack("<4s16xIII", b"DXBC", 1, length, len(parts))
    return header + struct.pack(f"<{len(parts)}I", *offsets) + b"".join(parts)


class ShaderContainerTests(unittest.TestCase):
    def test_program_flags_identify_only_the_declared_bitcode_field(self):
        for signature, label in [(b"NOBC", "NOBC"), (b"BC\xc0\xde", "LLVM signature")]:
            program = (
                struct.pack("<II4sIII", 0x10066, 7, b"DXIL", 0x106, 16, 4) + signature
            )
            image = b"DXBC" + container(part(b"PRIV", b"NOBC"), part(b"DXIL", program))
            sections = [{"name": ".data", "offset": 0, "raw_size": len(image)}]
            with self.subTest(signature=signature):
                report = scan(image, sections)
                self.assertEqual(report["containers_by_section"], {".data": 1})
                self.assertEqual(report["program_versions"], {"kind 1, model 6.6": 1})
                self.assertEqual(report["part_tags"], {"PRIV": 1, "DXIL": 1})
                self.assertEqual(report["bitcode_fields"], {label: 1})

    def test_container_offsets_must_fit_and_refer_to_disjoint_parts(self):
        image = bytearray(container(part(b"PRIV", b"abcd"), part(b"HASH", b"efgh")))
        self.assertIsNone(dxbc_parts(image, 0, len(image) - 1))
        struct.pack_into("<I", image, 36, 40)
        self.assertIsNone(dxbc_parts(image, 0, len(image)))

    def test_bitcode_offset_cannot_refer_to_the_header_or_next_part(self):
        for offset in (0, 20):
            program = (
                struct.pack("<II4sIII", 0x66, 7, b"DXIL", 0x106, offset, 4) + b"NOBC"
            )
            self.assertIsNone(inspect_program(program, 0, len(program)))
