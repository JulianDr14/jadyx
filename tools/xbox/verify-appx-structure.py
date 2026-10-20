# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check signed AXPC/AXCD and ZIP CRCs. Inputs are authenticated by verify-appx.ps1.

The unsigned ZIP directory is reconstructed by removing only the signature entry,
restoring counts and offsets in EOCD/ZIP64. No package bytes are rewritten.
"""
import hashlib,struct,sys,zipfile
from pathlib import Path

def verify(package,axpc,axcd):
    with zipfile.ZipFile(package) as archive, open(package,'rb') as stream:
        infos=archive.infolist()
        if len(infos)>10000: raise ValueError('Too many package entries')
        signature=archive.getinfo('AppxSignature.p7x')
        if signature.header_offset!=max(i.header_offset for i in infos):raise ValueError('Signature must be the final local record')
        digest=hashlib.sha256();remaining=signature.header_offset
        while remaining:
            chunk=stream.read(min(1048576,remaining))
            if not chunk:raise ValueError('Truncated package file records')
            digest.update(chunk);remaining-=len(chunk)
        if digest.hexdigest()!=axpc.lower():raise ValueError('Signed AXPC file-record digest mismatch')
        stream.seek(archive.start_dir);central=bytearray();count=0;signature_count=0
        while True:
            marker=stream.read(4)
            if marker!=b'PK\x01\x02':break
            header=stream.read(42)
            if len(header)!=42:raise ValueError('Truncated central directory')
            n,e,c=struct.unpack_from('<HHH',header,24)
            body=stream.read(n+e+c)
            if len(body)!=n+e+c:raise ValueError('Truncated directory entry')
            if body[:n]==b'AppxSignature.p7x':signature_count+=1
            else:central.extend(marker+header+body);count+=1
            if len(central)>(16<<20):raise ValueError('Central directory exceeds metadata bound')
        if signature_count!=1 or count!=len(infos)-1:raise ValueError('Invalid signature directory entry')
        tail=bytearray(marker+stream.read(131072))
        if len(tail)>=131072:raise ValueError('Oversized ZIP trailer')
        offset=0
        if tail[:4]==b'PK\x06\x06':
            record_size=struct.unpack_from('<Q',tail,4)[0]
            if record_size!=44 or tail[56:60]!=b'PK\x06\x07':raise ValueError('Unsupported ZIP64 trailer')
            struct.pack_into('<QQQQ',tail,24,count,count,len(central),signature.header_offset)
            struct.pack_into('<Q',tail,64,signature.header_offset+len(central));offset=76
        if tail[offset:offset+4]!=b'PK\x05\x06' or len(tail)!=offset+22+struct.unpack_from('<H',tail,offset+20)[0]:raise ValueError('Invalid EOCD trailer')
        for at,value,fmt,sentinel in [(8,count,'H',0xffff),(10,count,'H',0xffff),(12,len(central),'I',0xffffffff),(16,signature.header_offset,'I',0xffffffff)]:
            original=struct.unpack_from('<'+fmt,tail,offset+at)[0]
            if original!=sentinel:struct.pack_into('<'+fmt,tail,offset+at,value)
        if hashlib.sha256(central+tail).hexdigest()!=axcd.lower():raise ValueError('Signed AXCD directory digest mismatch')
        # Reading every entry also validates CRC and local/central filename consistency.
        for info in infos:
            with archive.open(info) as entry:
                while entry.read(1048576):pass
    return len(infos)

if __name__=='__main__':
    try:print('APPX signed container and CRC: OK, entries='+str(verify(Path(sys.argv[1]),sys.argv[2],sys.argv[3])))
    except Exception as error:
        print('APPX verification failed: '+str(error),file=sys.stderr);sys.exit(1)
