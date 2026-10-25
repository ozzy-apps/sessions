#!/usr/bin/env python3
import re
import sys
from pathlib import Path
version = re.search(r"project\(Sessions VERSION ([0-9.]+)", Path("CMakeLists.txt").read_text()).group(1)
if sys.argv[1] != "v" + version:
    raise SystemExit(f"Tag {sys.argv[1]} does not match CMake version {version}")
if not Path("LICENSE").is_file():
    raise SystemExit("Choose and add the project's software LICENSE before a public release.")
print("Release version and license present:", version)
