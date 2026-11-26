"""Content hashes cover full files and a failed audit preserves the old manifest."""

import contextlib
import hashlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts import audit_content


class ContentAuditTests(unittest.TestCase):
    def test_digest_includes_last_byte_after_a_full_buffer(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "input"
            (root / "nested").mkdir(parents=True)
            content = bytes(8 * 1024 * 1024) + b"!"
            (root / "nested" / "data.bin").write_bytes(content)
            (root / "empty.bin").touch()
            output = root.parent / "manifest.json"
            with contextlib.redirect_stdout(io.StringIO()):
                audit_content.audit(root, output)
            manifest = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(manifest["file_count"], 2)
            self.assertEqual(manifest["read_bytes"], len(content))
            self.assertEqual(manifest["by_extension"][".bin"]["bytes"], len(content))
            entries = {f["path"]: f["sha256"] for f in manifest["files"]}
            self.assertEqual(
                entries["nested/data.bin"], hashlib.sha256(content).hexdigest()
            )
            self.assertEqual(entries["empty.bin"], hashlib.sha256(b"").hexdigest())
            with self.assertRaises(ValueError):
                audit_content.audit(root, root / "manifest.json")

    def test_change_during_audit_preserves_previous_report(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "input"
            root.mkdir()
            (root / "old").touch()
            output = root.parent / "manifest.json"
            output.write_text("previous", encoding="ascii")
            first = audit_content.index_files(root)
            with patch.object(audit_content, "index_files", side_effect=[first, {}]):
                with (
                    contextlib.redirect_stdout(io.StringIO()),
                    self.assertRaises(RuntimeError),
                ):
                    audit_content.audit(root, output)
            self.assertEqual(output.read_text(encoding="ascii"), "previous")
