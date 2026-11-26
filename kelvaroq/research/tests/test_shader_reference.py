"""Keep PE framing separate from anything the reference crate consumes."""

import struct
import unittest

from research.check_shader_reference import qualification, shader_inputs
from research.tests.test_embedded_shaders import container, part


class ShaderReferenceTests(unittest.TestCase):
    def test_pipe_contains_only_complete_section_containers(self):
        program = struct.pack("<II4sIII", 0x10066, 7, b"DXIL", 0x106, 16, 4) + b"NOBC"
        vertex = container(part(b"PRIV", bytes(12)), part(b"DXIL", program))
        other = container(part(b"TEST", b"abcd"))
        prefix = b"outside-section"
        padding = b"DXBCpadding"
        section = padding + vertex + bytes(3) + other
        data = prefix + section + vertex
        sources, packet = shader_inputs(
            data,
            [
                {
                    "rva": 0x2000,
                    "offset": len(prefix),
                    "raw_size": len(section),
                }
            ],
        )
        first = 0x2000 + len(padding)
        second = first + len(vertex) + 3
        self.assertEqual(
            sources,
            [
                {
                    "rva": hex(first),
                    "version": (1, 6, 6),
                    "bitcode": "NOBC",
                    "private_bytes": 12,
                },
                {
                    "rva": hex(second),
                    "version": None,
                    "bitcode": None,
                    "private_bytes": None,
                },
            ],
        )
        self.assertEqual(
            packet,
            struct.pack("<II", first, len(vertex))
            + vertex
            + struct.pack("<II", second, len(other))
            + other,
        )

    def test_section_boundary_rejects_truncated_container(self):
        shader = container(part(b"TEST", b"abcd"))
        section = {"rva": 0x1000, "offset": 0, "raw_size": len(shader) - 1}
        self.assertEqual(shader_inputs(shader, [section]), ([], bytearray()))
        section["raw_size"] = len(shader) + 1
        with self.assertRaisesRegex(ValueError, "section range"):
            shader_inputs(shader, [section])

    def test_qualification_distinguishes_stage_pso_and_pixel_results(self):
        sources = [{"rva": hex(i + 1), "bitcode": "NOBC"} for i in range(10)]
        rows = []

        def add(rva, step, ok=True, detail=""):
            rows.append({"rva": rva, "step": step, "ok": ok, "detail": detail})

        for i, source in enumerate(sources):
            rva = source["rva"]
            for step in ("container", "native", "decoded", "translation"):
                add(rva, step)
            add(rva, "stage", detail="Vs" if i == 0 else "Ps")
            add(rva, "dxil", False, "malformed shader: bitcode lacks LLVM magic")
            if i:
                add(rva, "pipeline")
        add("0x0", "adapter")
        self.assertTrue(all(qualification(sources, rows, True, False).values()))
        self.assertFalse(qualification(sources, rows, True, True)["pixel_readback"])
        add("0x5", "pixels")
        self.assertTrue(all(qualification(sources, rows, True, True).values()))
        rows.append(rows[0])
        self.assertFalse(qualification(sources, rows, True, True)["container"])
