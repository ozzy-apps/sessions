#!/usr/bin/env python3
"""Reproduce the bundled CC-BY-SA Sessions Acoustic Collection from pinned archives."""
from pathlib import Path
import shutil,json,hashlib
# Run from the repository root. Assets are already bundled; this only reproduces them.
import urllib.request, tempfile, zipfile
archives = {
    'BLACK_PEARL_2023_repack.zip': '1ab43270ce4ea5d8794e98b158210a566e431d7e76646ef778ebabafba807609',
    'RED_ZEPPELIN_2023_repack.zip': '9f5c5e5a76decae7eb63b2242f91a51210e91f7e5bb57587352c544721b1012f',
    'BLONDE_BOP_SFZ.zip': 'a89629acd89a888e628bc790e1d3406bb03f6b9ad9b7f634298b23d79c8c5d2d',
}
staging = Path(tempfile.mkdtemp(prefix='sessions-drum-sources-'))
for archive, checksum in archives.items():
    payload = urllib.request.urlopen('https://www.bandshed.net/sounds/AVLDrumkits_SFZ/' + archive).read()
    if hashlib.sha256(payload).hexdigest() != checksum:
        raise RuntimeError('Upstream archive changed: ' + archive)
    target = staging / archive
    target.write_bytes(payload)
    with zipfile.ZipFile(target) as z:
        for entry in z.infolist():
            path = (staging / entry.filename).resolve()
            if not path.is_relative_to(staging):
                raise RuntimeError('Unsafe archive path')
        z.extractall(staging)
root=Path('assets/drums')
banks=[]
for key,folder,name,mapping in [
 ('pearl','BLACK_PEARL_2023_repack','Black Pearl',{52:62}),
 ('zeppelin','RED_ZEPPELIN_2023_repack','Red Zeppelin',{52:62}),
 ('bop','BLONDE_BOP_SFZ','Blonde Bop',{52:60})]:
 src=staging/folder; out=root/key;out.mkdir(exist_ok=True)
 bank={'id':key,'name':name,'notes':{}}
 for gm in [36,37,38,39,40,41,43,42,44,45,46,47,49,51,52,53,55,57,59]:
  files=sorted(src.glob(str(mapping.get(gm,gm))+'-*-?.wav'))
  assert len(files)==5,(gm,files)
  bank['notes'][str(gm)]=[]
  for f in files:
   shutil.copyfile(f,out/f.name)
   bank['notes'][str(gm)].append(key+'/'+f.name)
 banks.append(bank)
(root/'library.json').write_text(json.dumps({'name':'Sessions Acoustic Collection','banks':banks},indent=2)+'\n')
print(sum(f.stat().st_size for f in root.rglob('*.wav'))/1e6,'MB',len(list(root.rglob('*.wav'))),'samples')

shutil.rmtree(staging)
