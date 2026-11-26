"""Benchmark native byte comparisons against the pinned backend in a local clone."""

import argparse
import os
import subprocess
import tempfile
from pathlib import Path

from scripts.build_native import REVISION


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("source", type=Path, help="local native backend Git clone")
    args = cli.parse_args()
    root = Path(__file__).resolve().parent
    raw = subprocess.check_output(
        [
            "git",
            "-C",
            args.source,
            "show",
            f"{REVISION}:crates/victory-gpu/src/shadow.rs",
        ],
        encoding="utf-8",
    )
    first = raw.index("fn first_difference(")
    end = raw.index("#[cfg(test)]", first)
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        old = work / "reference.rs"
        old.write_text(
            raw[first:end]
            .replace("fn first_difference(", "pub fn first_difference(")
            .replace("fn last_difference(", "pub fn last_difference("),
            encoding="utf-8",
        )
        exe = work / ("compare.exe" if os.name == "nt" else "compare")
        subprocess.run(
            [
                "rustc",
                "--edition",
                "2024",
                "-O",
                root / "bench_shadow_compare.rs",
                "-o",
                exe,
            ],
            check=True,
            env=os.environ | {"SHADOW_REFERENCE": str(old)},
        )
        subprocess.run([exe], check=True)


if __name__ == "__main__":
    main()
