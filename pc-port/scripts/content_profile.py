# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate local package presence and parse bounded PlayGo chunk sizes; no asset copies."""
import hashlib
import json
from pathlib import Path
import struct
import re
import xml.etree.ElementTree as ET
from prepare import ROOT, DEFAULT_GAME

def create():
    game=DEFAULT_GAME.resolve()
    manifest=game/'Project.gp4'
    # Dumps without a GP4 manifest have no declared file list: treat them as complete.
    paths=[n.get('targ_path') for n in ET.parse(manifest).findall('./files/file')] if manifest.is_file() else []
    missing=[]
    total=0
    for relative in paths:
        p=(game/relative).resolve()
        p.relative_to(game)
        if not p.is_file(): missing.append(relative)
        else: total+=p.stat().st_size
    source=game/'sce_sys/playgo-chunk.dat'
    data=source.read_bytes()
    magic,major,minor,images,chunks,mchunks,scenarios,size=struct.unpack_from('<I6HI',data)
    if magic!=0x6f676c70 or not 0<chunks<=1000 or not 0<mchunks<=8000 or size!=len(data):
        raise ValueError('unsupported PlayGo header')
    tables=[]
    for i in range(8):
        offset,length=struct.unpack_from('<II',data,192+i*8)
        if offset+length>len(data): raise ValueError('PlayGo table outside file')
        tables.append(data[offset:offset+length])
    sizes=[]
    for i in range(chunks):
        count=struct.unpack_from('<H',tables[0],i*32+14)[0]
        offset=struct.unpack_from('<I',tables[0],i*32+24)[0]
        ids=struct.unpack_from('<'+'H'*count,tables[1],offset)
        if any(idx>=mchunks for idx in ids): raise ValueError('invalid microchunk ID')
        sizes.append(sum(struct.unpack_from('<Q',tables[3],idx*16+8)[0]&0xffffffffffff for idx in ids))
    report={'manifest':str(manifest),'manifest_sha256':hashlib.sha256(manifest.read_bytes()).hexdigest() if manifest.is_file() else None,
            'declared_files':len(paths),'present_file_bytes':total,'missing':missing,
            'playgo_sha256':hashlib.sha256(data).hexdigest(),'chunks':chunks,'chunk_sizes':sizes,
            'availability_basis':'All files declared by the local GP4 manifest are present; not an asset hash audit.'}
    out=ROOT/'out'
    trophy=(game/'sce_sys/trophy/trophy00.trp').read_bytes()
    magic,version,file_size,entry_count,entry_size=struct.unpack_from('>IIQII',trophy)
    if magic!=0xdca24d00 or version!=3 or file_size!=len(trophy) or entry_size!=64 or 96+entry_count*entry_size>len(trophy):
        raise ValueError('unsupported trophy table')
    trophy_ids=[]
    for i in range(entry_count):
        name=trophy[96+i*64:128+i*64].split(b'\0')[0].decode('ascii')
        match=re.fullmatch(r'TROP(\d{3})\.PNG',name)
        if match:trophy_ids.append(int(match[1]))
    if not trophy_ids or max(trophy_ids)>=128:raise ValueError('unsupported trophy IDs')
    report['trophy_ids']=trophy_ids
    (out/'content-profile.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    (out/'content_profile.h').write_text('/* Generated from local package metadata; no assets. */\n'
        + f'#define LOCAL_PACKAGE_COMPLETE {int(not missing)}\n#define LOCAL_CHUNKS {chunks}\n'
        + 'static const uint64_t chunk_sizes[LOCAL_CHUNKS]={'+','.join(str(n)+'ULL' for n in sizes)+'};\n'
        + f'#define LOCAL_TROPHY_COUNT {len(trophy_ids)}\n'
        + 'static const unsigned char trophy_ids[LOCAL_TROPHY_COUNT]={'+','.join(map(str,trophy_ids))+'};\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k!='chunk_sizes'},indent=2))

if __name__=='__main__': create()
