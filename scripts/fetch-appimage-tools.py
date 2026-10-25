#!/usr/bin/env python3
"""Fetch packaging executables and require the reviewed hashes in the lock file."""
import hashlib
import json
from pathlib import Path
import sys
import urllib.request
root = Path(__file__).resolve().parents[1]
destination = Path(sys.argv[1] if len(sys.argv) > 1 else root / "build/release-tools")
destination.mkdir(parents=True, exist_ok=True)
for name, info in json.loads((root / "distribution/appimage-tools.json").read_text()).items():
    path = destination / name
    if not path.exists():
        with urllib.request.urlopen(info["url"], timeout=60) as response, path.open("wb") as output:
            while chunk := response.read(1024 * 1024):
                output.write(chunk)
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != info["sha256"]:
        raise SystemExit(f"Checksum mismatch for {name}; review the upstream release and update distribution/appimage-tools.json.")
    path.chmod(0o755)
    print("Verified", name)
