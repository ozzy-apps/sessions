#!/usr/bin/env python3
"""Build an OS-independent, attributed sample pack without recompressing FLAC."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1] / "content/drums")
parser.add_argument("--output", type=Path, default=Path("dist/sessions-drum-content-v1.zip"))
args = parser.parse_args()
manifest = json.loads((args.root / "library.json").read_text())
if not manifest.get("banks") or not any((args.root / "audio").rglob("*.flac")):
    raise SystemExit("Fetch and prepare the drum content before packaging.")
args.output.parent.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(args.output, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as archive:
    for source in sorted(args.root.rglob("*")):
        if source.is_file() and not source.is_symlink():
            archive.write(source, "drum-content/" + source.relative_to(args.root).as_posix())
digest = hashlib.file_digest(args.output.open("rb"), "sha256").hexdigest()
args.output.with_suffix(args.output.suffix + ".sha256").write_text(f"{digest}  {args.output.name}\n")
print(f"{args.output}: {args.output.stat().st_size:,} bytes; SHA256 {digest}")
