"""Read a directory in full and write a SHA-256 manifest outside the input tree."""

import argparse
import hashlib
import json
import os
import stat
import sys
import tempfile
import time
from collections import defaultdict
from pathlib import Path


def index_files(root):
    def visit_error(error):
        raise error

    result = {}
    for directory, children, names in os.walk(root, onerror=visit_error):
        directory = Path(directory)
        for name in children:
            child = directory / name
            if child.is_symlink() or bool(
                getattr(child.lstat(), "st_file_attributes", 0)
                & stat.FILE_ATTRIBUTE_REPARSE_POINT
            ):
                raise ValueError(f"linked directory not supported: {child}")
        for name in names:
            child = directory / name
            info = child.lstat()
            if not stat.S_ISREG(info.st_mode) or child.is_symlink():
                raise ValueError(f"expected a regular file: {child}")
            result[child.relative_to(root).as_posix()] = signature(info)
    return result


def signature(info):
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns


def audit(root, output):
    root, output = root.resolve(strict=True), output.resolve()
    if not root.is_dir() or output.is_relative_to(root):
        raise ValueError("choose an input directory and a manifest outside it")
    index = index_files(root)
    total = sum(entry[2] for entry in index.values())
    print(f"Reading {len(index)} files, {total:,} bytes", flush=True)
    records = []
    types = defaultdict(lambda: {"files": 0, "bytes": 0})
    processed = 0
    last_update = time.monotonic()
    buffer = bytearray(8 * 1024 * 1024)
    view = memoryview(buffer)
    for name, expected in sorted(index.items()):
        digest = hashlib.sha256()
        count = 0
        with (root / name).open("rb", buffering=0) as stream:
            if signature(os.fstat(stream.fileno())) != expected:
                raise RuntimeError(f"file changed before reading: {name}")
            while size := stream.readinto(buffer):
                digest.update(view[:size])
                count += size
                processed += size
                if time.monotonic() - last_update >= 15:
                    print(f"{processed:,} / {total:,} bytes", flush=True)
                    last_update = time.monotonic()
            if count != expected[2] or signature(os.fstat(stream.fileno())) != expected:
                raise RuntimeError(f"file changed while reading: {name}")
        records.append({"path": name, "size": count, "sha256": digest.hexdigest()})
        kind = types[Path(name).suffix.lower()]
        kind["files"] += 1
        kind["bytes"] += count
    if index_files(root) != index:
        raise RuntimeError("directory changed during the audit")
    report = {
        "schema": 1,
        "algorithm": "SHA-256",
        "read_bytes": processed,
        "file_count": len(records),
        "by_extension": dict(types),
        "files": records,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=output.parent, delete=False) as stream:
        temp = Path(stream.name)
    try:
        temp.write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
        )
        os.replace(temp, output)
    finally:
        temp.unlink(missing_ok=True)
    print(f"Complete: {len(records)} files, {processed:,} bytes", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args()
    try:
        audit(args.directory, args.manifest)
    except (OSError, RuntimeError, ValueError) as error:
        sys.exit(str(error))
