#!/usr/bin/env python3
"""Fetch checksum-pinned, MIT Spleeter model weights and the official CPU ONNX runtime."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tarfile
import urllib.request
import zipfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--platform', choices=['linux', 'windows', 'all'], default='windows' if sys.platform == 'win32' else 'linux')
parser.add_argument('--destination', type=Path, default=root / 'build/stems-assets')
parser.add_argument('--no-models', action='store_true')
args = parser.parse_args()
lock = json.loads((root / 'distribution/stems-assets.json').read_text())
destination = args.destination.resolve()
(destination / 'downloads').mkdir(parents=True, exist_ok=True)

def valid(path, info):
    return path.is_file() and path.stat().st_size == info['size'] and hashlib.sha256(path.read_bytes()).hexdigest() == info['sha256']

def fetch(info, path):
    if not valid(path, info):
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_name('.' + path.name + '.download')
        try:
            with urllib.request.urlopen(info['url'], timeout=60) as response, temporary.open('wb') as output:
                shutil.copyfileobj(response, output)
            if not valid(temporary, info):
                raise RuntimeError('Checksum/size mismatch: ' + info['file'])
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)
    print('Verified', path.name, flush=True)
    return path

if not args.no_models:
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        list(pool.map(lambda info: fetch(info, destination / 'models' / info['file']), lock['models']))
for platform in (['linux', 'windows'] if args.platform == 'all' else [args.platform]):
    info = lock['runtime'][platform]
    archive = fetch(info, destination / 'downloads' / info['file'])
    target = destination / 'runtime' / platform
    target.mkdir(parents=True, exist_ok=True)
    if archive.suffix == '.zip':
        with zipfile.ZipFile(archive) as z:
            for name in z.namelist():
                path = Path(name)
                if ('/lib/' in name and path.suffix == '.dll') or path.name in ['LICENSE', 'ThirdPartyNotices.txt']:
                    (target / path.name).write_bytes(z.read(name))
    else:
        with tarfile.open(archive) as t:
            for member in t.getmembers():
                path = Path(member.name)
                if member.isfile() and '/lib/' in member.name and ('.so.' in path.name or path.name.endswith('.so')):
                    with t.extractfile(member) as stream:
                        (target / path.name).write_bytes(stream.read())
                # Keep the runtime's complete notices alongside each packaged platform.
                if member.isfile() and path.name in ['LICENSE', 'ThirdPartyNotices.txt']:
                    (target / path.name).write_bytes(t.extractfile(member).read())
    if not (target / info['library']).is_file():
        raise RuntimeError('Runtime library absent: ' + info['library'])
shutil.copy2(root / 'distribution/stems-assets.json', destination / 'assets.json')
print('Stems assets ready:', destination)
