#!/usr/bin/env python3
"""Prepare native GM mappings from the downloaded CC0/CC-BY drum recordings.

Copies unmodified source recordings and license notices. This is a bounded mapping
converter, not an SFZ player; unsupported upstream performance/effect scripts are not run.
"""
from pathlib import Path
from collections import defaultdict
import re,json,shutil,hashlib,os
ROOT=Path(__file__).resolve().parents[1];SOURCE=ROOT/'dist/drum-sources';OUT=ROOT/'content/drums'
OUT.mkdir(parents=True,exist_ok=True)
manifest={'version':1,'banks':[]};checksums={}

def expand_sfz(path,base,macros=None,depth=0):
    if depth>20:raise ValueError('Include depth')
    if macros is None:macros={}
    result=[]
    def sub(s):
        if not macros:return s
        pat='|'.join(re.escape(k) for k in sorted(macros,key=len,reverse=True))
        for _ in range(4):
            new=re.sub(pat,lambda m:macros[m.group()],s)
            if new==s:break
            s=new
        return s
    for raw in path.read_text(errors='strict').splitlines():
        line=raw.split('//',1)[0].strip()
        if not line:continue
        if '#define' in line:
            for m in re.finditer(r'#define\s+(\$\w+)\s+(.+?)(?=\s*#define|$)',line):macros[m[1]]=sub(m[2].strip())
            continue
        if '#include' in line:
            before,include=line.split('#include',1)
            if before.strip():result.append(sub(before))
            name=re.search(r'"([^"]+)"',include)[1];target=(base/sub(name)).resolve()
            if not target.is_relative_to(SOURCE.resolve()):raise ValueError('Unsafe include')
            result.extend(expand_sfz(target,base,macros,depth+1));continue
        if line.startswith('#'):raise ValueError('Unsupported preprocessor directive '+line)
        result.append(sub(line))
    return result

def regions(path,base=None):
    base=base or path.parent
    text='\n'.join(expand_sfz(path,base));global_,master,group,region={},{},{},None;target=global_;out=[]
    for token in re.split(r'(<[^>]+>)',text):
        if token.startswith('<'):
            if region is not None:out.append(region);region=None
            kind=token[1:-1].strip()
            if kind=='global':global_={};master={};group={};target=global_
            elif kind=='master':master=dict(global_);group={};target=master
            elif kind=='group':group={**global_,**master};target=group
            elif kind=='region':region={**global_,**master,**group};target=region
            else:target={}
        else:
            for match in re.finditer(r'([a-zA-Z0-9_]+)\s*=\s*(.*?)(?=\s+[a-zA-Z0-9_]+\s*=|$)',token,re.S):target[match[1]]=match[2].strip()
    if region is not None:out.append(region)
    return out

def table(path,sample_root=None,base=None):
    result={}
    for r in regions(path,base):
        if not r.get('sample') or r['sample'].startswith('*'):continue
        low=int(float(r.get('lovel','1')));high=int(float(r.get('hivel','127')))
        rr=int(r.get('seq_position','1'))
        if rr==1:
            match=re.search(r'_rr(\d+)',r['sample'])
            if match:rr=int(match[1])
        file=(sample_root/r['sample'] if sample_root else (base or path.parent)/r['sample']).resolve()
        if not file.is_file():raise FileNotFoundError(file)
        key=int(r.get('key','0')) if r.get('key','0').lstrip('-').isdigit() else 0
        result.setdefault((key,low,high,rr),file)
    return result

def copy(path,slug,repo):
    if path is None:return None
    relative=path.relative_to(SOURCE/repo);target=OUT/'audio'/slug/relative;target.parent.mkdir(parents=True,exist_ok=True)
    if not target.exists():
        try:os.link(path,target)
        except OSError:shutil.copyfile(path,target)
    ref=target.relative_to(OUT).as_posix()
    if ref not in checksums:
        h=hashlib.sha256()
        with target.open('rb') as f:
            for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
        checksums[ref]=h.hexdigest()
    return ref

def add_bank(slug,name,repo,license,author,note_maps):
    notes={}
    for gm,channels in note_maps.items():
        close=channels['close'];groups=defaultdict(list)
        for key,file in sorted(close.items()):
            _,low,high,rr=key;hit={'close':copy(file,slug,repo)}
            for channel,other in channels.items():
                if channel=='close':continue
                if isinstance(other,list):
                    pair=[m.get(key) for m in other]
                    if all(pair):hit[channel]=[copy(p,slug,repo) for p in pair]
                elif key in other:hit[channel]=copy(other[key],slug,repo)
            groups[(low,high)].append(hit)
        notes[str(gm)]=[{'low':lo,'high':hi,'hits':hits} for (lo,hi),hits in sorted(groups.items())]
    notice=OUT/'licenses'/slug;notice.mkdir(parents=True,exist_ok=True)
    for f in (SOURCE/repo).iterdir():
        if f.is_file() and any(t in f.name.lower() for t in ['license','readme','guide']):shutil.copyfile(f,notice/f.name)
    provenance=SOURCE/(repo+'.provenance.json');shutil.copyfile(provenance,notice/'provenance.json')
    bank={'id':slug,'name':name,'license':license,'author':author,'source':'https://github.com/sfzinstruments/'+repo,'notes':notes}
    manifest['banks'].append(bank);print(name,len(notes),'articulations',sum(len(g['hits']) for groups in notes.values() for g in groups),'recorded strikes',flush=True)

# Main GM articulations; explicitly selected source folders retain velocity/RR recordings.
repo='karoryfer.big-rusty-drums'
# Upstream leaf names use compact instrument prefixes; locate via the sample directory paths.
def sample_groups(folder):
    files=sorted(p for p in folder.glob('*') if p.suffix.lower() in ('.flac','.wav'))
    groups=defaultdict(list)
    for p in files:
        if folder.name=='marching_kick' and '_reso' not in p.name:continue
        m=re.search(r'_vl(\d+)_rr(\d+)',p.name)
        if m:groups[int(m[1])].append((int(m[2]),p))
    result={};count=max(groups,default=1)
    for v,files in sorted(groups.items()):
        lo=1 if v==1 else int((v-1)*127/count)+1;hi=int(v*127/count)
        for rr,p in files:result[(0,lo,hi,rr)]=p
    if not result:raise RuntimeError('No velocity/round-robin files in '+str(folder))
    return result
# Explicit author source folders make the remapping inspectable.
big_dirs={36:'kick_24/kick',37:'snare_14/sidestick',38:'snare_14/center',40:'snare_14/rimshot',41:'tom_22/center',43:'tom_18/center',45:'tom_15/center',47:'tom_14/center',48:'tom_14/center',42:'hihat_14/cl',44:'hihat_14/chik',46:'hihat_14/open',49:'crash_17/cr',57:'crash_sizzle_17/cr',51:'ride_22/rd',53:'ride_22/bl',59:'ride_22/rd',52:'china_18/cn'}
notes={}
for gm,directory in big_dirs.items():
    root=SOURCE/repo/'Samples'/directory;primary='kick' if gm==36 else 'top' if gm in (37,38,40) else 'cl'
    channels={'close':sample_groups(root/primary),'overhead':sample_groups(root/'oh')}
    alt=root/('btm' if gm in (37,38,40) else 'sn' if gm==36 else 'cl')
    if alt.is_dir() and alt!=root/primary:channels['alternate']=sample_groups(alt)
    notes[gm]=channels
add_bank('big-rusty','Big Rusty Drums',repo,'CC0-1.0','Karoryfer Samples',notes)
repo='karoryfer.swirly-drums';dirs={36:'marching_kick',37:'snare_edge',38:'snare_main',40:'snare_edge',41:'tom_floor',43:'tom_mlow',45:'tom_xmed',47:'tom_mhi',48:'tom_vhi',42:'hat_closed',44:'hat_foot',46:'hat_open',49:'crash',57:'crash_broken',51:'ride',53:'ride',59:'ride',55:'splash',52:'china'}
notes={}
for gm,directory in dirs.items():
    t=sample_groups(SOURCE/repo/'Samples'/directory)
    # The kick has reso/batter variants with matching hit names. Keep its reso recording as primary.
    if gm==36:
        t={k:p for k,p in t.items() if 'reso' in p.name}
        if not t:t=sample_groups(SOURCE/repo/'Samples'/directory)
    notes[gm]={'close':t}
add_bank('swirly','Swirly Drums',repo,'CC0-1.0','Karoryfer Samples',notes)
repo='karoryfer.frankensnare';notes={}
for gm,directory in {37:'12_basic',38:'14p_basic',40:'14a_rimshot'}.items():
    root=SOURCE/repo/'Samples'/directory;notes[gm]={'close':sample_groups(root/'top'),'alternate':sample_groups(root/'btm'),'overhead':sample_groups(root/'oh')}
add_bank('frankensnare','Frankensnare',repo,'CC0-1.0','Karoryfer Samples',notes)

def mic_table(root,filename):return table(root/'Multi'/filename,root/'Samples',root/'Multi')
def filter_note(t,note):return {(0,*key[1:]):p for key,p in t.items() if key[0]==note}
for repo,sub,slug,name,gmmap,mics in [
 ('DrumGizmo.DRSKit','DrumGizmo/DRSKit','drs','DRSKit',{36:36,37:37,38:38,40:40,41:41,43:41,45:43,47:43,48:47,42:42,44:44,46:58,49:49,57:57,51:51,53:53,59:66},
 {36:('Kickdrum back.sfz','Kickdrum front.sfz'),38:('Snaredrum top.sfz','Snaredrum bottom.sfz'),37:('Snaredrum top.sfz','Snaredrum bottom.sfz'),40:('Snaredrum top.sfz','Snaredrum bottom.sfz'),41:('Tom3 (Floor tom).sfz',None),43:('Tom3 (Floor tom).sfz',None),45:('Tom2 (Floor tom).sfz',None),47:('Tom2 (Floor tom).sfz',None),48:('Tom1.sfz',None),42:('Hihat.sfz',None),44:('Hihat.sfz',None),46:('Hihat.sfz',None),51:('Ride cymbal.sfz',None),53:('Ride cymbal.sfz',None),59:('Ride cymbal.sfz',None)}),
 ('DrumGizmo.MuldjordKit','DrumGizmo/MuldjordKit','muldjord','MuldjordKit',{36:35,37:37,38:38,40:38,41:41,43:41,45:47,47:47,48:48,42:42,44:42,46:46,49:49,57:57,51:51,53:53,59:59,52:52},
 {36:('Kickdrum left.sfz',None),38:('Snare top.sfz','Snare bottom.sfz'),37:('Snare top.sfz','Snare bottom.sfz'),40:('Snare top.sfz','Snare bottom.sfz'),41:('Floor tom.sfz',None),43:('Floor tom.sfz',None),45:('Rack tom 2.sfz',None),47:('Rack tom 2.sfz',None),48:('Rack tom 1.sfz',None),42:('Hihat.sfz',None),44:('Hihat.sfz',None),46:('Hihat.sfz',None),51:('Ride right.sfz',None),53:('Ride right.sfz',None),59:('Ride left.sfz',None)})]:
    root=SOURCE/repo/sub
    if slug=='drs': # use the no-whiskers map and its corresponding microphone filenames
        multi=root/'Multi-N'
    else:multi=root/'Multi'
    cache={}
    def get(filename):
        if filename not in cache:cache[filename]=table(multi/filename,root/'Samples',multi)
        return cache[filename]
    ohl=get('Overhead left.sfz');ohr=get('Overhead right.sfz');rl=get('Ambience left.sfz');rr=get('Ambience right.sfz');notes={}
    for gm,native in gmmap.items():
        primary,alternate=mics.get(gm,('Overhead left.sfz',None));cl=filter_note(get(primary),native)
        if not cl:continue
        channels={'close':cl,'overhead':[filter_note(ohl,native),filter_note(ohr,native)],'room':[filter_note(rl,native),filter_note(rr,native)]}
        if alternate:channels['alternate']=filter_note(get(alternate),native)
        notes[gm]=channels
    add_bank(slug,name,repo,'CC-BY-4.0','Lars Muldjord / Bent Bisballe Nyeng; SFZ mapping by kinwie',notes)
# Naked Drums: source files carry all microphone pairings and exact velocity/sequence opcodes.
repo='WilkinsonAudio.NakedDrums';root=SOURCE/repo/'Wilkinson Audio/Naked Drums';cache={}
def naked(filename):
    if filename not in cache:cache[filename]=mic_table(root,filename)
    return cache[filename]
gmmap={36:36,37:37,38:38,40:40,41:41,43:43,45:45,47:47,48:48,42:42,44:44,46:49,49:55,57:57,51:51,53:53,59:51,55:54,52:52}
primary={36:'Kick In.sfz',37:'Snare Tops.sfz',38:'Snare Tops.sfz',40:'Snare Tops.sfz',41:'Tom 5.sfz',43:'Tom 4.sfz',45:'Tom 3.sfz',47:'Tom 2.sfz',48:'Tom 1.sfz',42:'Hats.sfz',44:'Hats.sfz',46:'Hats.sfz',51:'Ride.sfz',53:'Ride.sfz',59:'Ride.sfz'}
notes={}
for gm,native in gmmap.items():
    cl=filter_note(naked(primary.get(gm,'Overheads.sfz')),native)
    if not cl:continue
    channels={'close':cl,'overhead':filter_note(naked('Overheads.sfz'),native),'room':filter_note(naked('Far Rooms.sfz'),native)}
    if gm in (37,38,40):channels['alternate']=filter_note(naked('Snare Bottom.sfz'),native)
    elif gm==36:channels['alternate']=filter_note(naked('Kick Sub.sfz'),native)
    notes[gm]=channels
# Place bank at stable position 4: the loop above appended Muldjord first.
add_bank('naked','Naked Drums',repo,'CC-BY-4.0','Wilkinson Audio, 2016; SFZ mapping by kinwie',notes)
manifest['banks'][4],manifest['banks'][5]=manifest['banks'][5],manifest['banks'][4]
# Remove obsolete generated mappings' audio copies; never touch upstream downloads.
for file in (OUT/'audio').rglob('*'):
    if file.is_file() and file.relative_to(OUT).as_posix() not in checksums:file.unlink()
(OUT/'library.json').write_text(json.dumps(manifest,indent=2)+'\n')
(OUT/'SHA256SUMS').write_text(''.join(f'{digest}  {path}\n' for path,digest in sorted(checksums.items())))
(OUT/'README.md').write_text('''# Sessions drum content\n\nNative GM mappings of the six approved libraries. Audio is copied without modification. Original license notices and exact source provenance are in licenses/. CC0 banks: Karoryfer Big Rusty Drums, Swirly Drums and Frankensnare. CC BY 4.0 banks: DrumGizmo DRSKit and MuldjordKit, and Wilkinson Audio Naked Drums, including SFZ mappings by kinwie.\n\nSessions maps the main GM articulations and retains their recorded velocity layers and round robins. Close/alternate/overhead/room recordings are synchronized by velocity and sequence. Unmapped GM articulations fall back to the original recipe; Frankensnare is a snare expansion. Arbitrary SFZ scripts/effects, CC-driven articulations and all optional upstream kit-piece variations are not interpreted. Full upstream downloads remain in dist/drum-sources for additional sample use.\n''')
print(len(checksums),'unique unmodified audio files,',sum((OUT/p).stat().st_size for p in checksums)/1e6,'MB',flush=True)
