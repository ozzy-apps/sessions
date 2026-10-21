#!/usr/bin/env python3
"""Download the six approved upstream drum libraries. Resume verified files; never run pack code.

Original archives/checkouts stay in dist/drum-sources. Content preparation is a separate step.
"""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib, json, os, subprocess, time, urllib.request, zipfile
ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / 'dist' / 'drum-sources'
DEST.mkdir(parents=True, exist_ok=True)
REPOS = ['karoryfer.big-rusty-drums', 'karoryfer.swirly-drums', 'karoryfer.frankensnare',
         'DrumGizmo.DRSKit', 'WilkinsonAudio.NakedDrums', 'DrumGizmo.MuldjordKit']
def request(url, payload=None):
    data = None if payload is None else json.dumps(payload).encode()
    headers = {'User-Agent': 'Sessions-drum-content/0.41', 'Accept': 'application/vnd.git-lfs+json' if data else 'application/vnd.github+json'}
    if data: headers['Content-Type'] = 'application/vnd.git-lfs+json'
    return urllib.request.urlopen(urllib.request.Request(url, data=data, headers=headers), timeout=120)
def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1024*1024), b''): h.update(chunk)
    return h.hexdigest()
def download(url, path, sha=None, headers=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and (not sha or digest(path)==sha): return
    tmp=path.with_name(path.name+'.part')
    for retry in range(4):
        try:
            req=urllib.request.Request(url,headers={'User-Agent':'Sessions-drum-content/0.41',**(headers or {})})
            with urllib.request.urlopen(req, timeout=120) as response, tmp.open('wb') as f:
                while chunk:=response.read(1024*1024): f.write(chunk)
            if sha and digest(tmp)!=sha: raise RuntimeError('SHA256 mismatch: '+str(path))
            tmp.replace(path); return
        except Exception:
            if retry==3: raise
            time.sleep(2*(retry+1))
def fetch(repo):
    url='https://api.github.com/repos/sfzinstruments/'+repo
    pinned=next((json.loads(p.read_text()) for p in (ROOT/'content/drums/licenses').glob('*/provenance.json') if json.loads(p.read_text()).get('repo')==repo),{})
    releases=[json.load(request(url+'/releases/tags/'+pinned['tag']))] if pinned.get('tag') else [] if pinned.get('commit') else json.load(request(url+'/releases'))
    provenance={'repo':repo,'url':'https://github.com/sfzinstruments/'+repo}
    if releases and releases[0]['assets']:
        release=releases[0]; asset=next(a for a in release['assets'] if a['name']==pinned['archive']) if pinned.get('archive') else next(a for a in release['assets'] if a['name'].lower().endswith('.zip'))
        archive=DEST/asset['name']; sha=pinned.get('sha256') or (asset.get('digest') or '').removeprefix('sha256:') or None
        print('Download', repo, asset['size'], flush=True); download(asset['browser_download_url'],archive,sha)
        provenance.update(tag=release['tag_name'],archive=asset['name'],sha256=digest(archive),bytes=archive.stat().st_size)
        out=DEST/repo; marker=out/'.extracted'
        if marker.exists() and marker.read_text()!=provenance['sha256']:raise RuntimeError('Extracted source differs from pinned archive: '+repo)
        if not marker.exists():
            out.mkdir(exist_ok=True)
            with zipfile.ZipFile(archive) as z:
                for entry in z.infolist():
                    target=(out/entry.filename).resolve()
                    if not target.is_relative_to(out.resolve()) or (entry.external_attr>>16)&0o170000==0o120000: raise RuntimeError('Unsafe ZIP entry')
                z.extractall(out)
            marker.write_text(provenance['sha256'])
    else:
        out=DEST/repo
        if not (out/'.git').exists():
            subprocess.run(['git','clone','--depth','1',provenance['url']+'.git',str(out)],check=True,env={**os.environ,'GIT_LFS_SKIP_SMUDGE':'1'},stdout=subprocess.DEVNULL)
            if pinned.get('commit'):
                subprocess.run(['git','-C',str(out),'fetch','--depth','1','origin',pinned['commit']],check=True,stdout=subprocess.DEVNULL)
                subprocess.run(['git','-C',str(out),'checkout','--detach',pinned['commit']],check=True,env={**os.environ,'GIT_LFS_SKIP_SMUDGE':'1'},stdout=subprocess.DEVNULL)
        commit=subprocess.check_output(['git','-C',str(out),'rev-parse','HEAD'],text=True).strip(); provenance['commit']=commit
        if pinned.get('commit') and commit!=pinned['commit']:raise RuntimeError('Existing checkout differs from pinned source: '+repo)
        pointers=[]
        for p in out.rglob('*'):
            if not p.is_file() or '.git' in p.relative_to(out).parts or p.stat().st_size>200: continue
            data=p.read_bytes()
            if data.startswith(b'version https://git-lfs.github.com/spec/v1'):
                fields=dict(line.split(' ',1) for line in data.decode().splitlines()[1:]); pointers.append((p,fields['oid'].split(':')[1],int(fields['size'])))
        def get_object(item, obj):
            p,sha,size=item
            action=obj.get('actions',{}).get('download')
            if not action: raise RuntimeError(str(obj))
            download(action['href'],p,sha,action.get('header'))
        print('LFS',repo,len(pointers),'objects',flush=True)
        for start in range(0,len(pointers),100):
            batch=pointers[start:start+100]
            result=json.load(request('https://github.com/sfzinstruments/'+repo+'.git/info/lfs/objects/batch',{'operation':'download','transfers':['basic'],'objects':[{'oid':sha,'size':size} for _,sha,size in batch]}))
            by_sha={obj['oid']:obj for obj in result['objects']}
            with ThreadPoolExecutor(max_workers=6) as pool:
                for f in as_completed([pool.submit(get_object,item,by_sha[item[1]]) for item in batch]): f.result()
            print(repo, min(start+100,len(pointers)),'/',len(pointers),flush=True)
    (DEST/(repo+'.provenance.json')).write_text(json.dumps(provenance,indent=2)+'\n')
    print('Ready',repo,flush=True)
with ThreadPoolExecutor(max_workers=3) as pool:
    for f in as_completed([pool.submit(fetch,repo) for repo in REPOS]): f.result()
print('All six drum libraries downloaded.',flush=True)
