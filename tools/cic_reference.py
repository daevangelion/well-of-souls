#!/usr/bin/env python3
# Reference CIC extractor mirroring cicdec 3.0.1 TryParse40 (installer v2.4->40).
# Ground truth from the real cicdec: 875 files, version 40. Validated vs extracted/.
import struct, zlib, bz2, sys, os

SIG=b'wwgT)H'; FILE_DATA=0x7F7F; FILE_LIST=0x143A
def u16(b,o): return struct.unpack_from('<H',b,o)[0]
def u32(b,o): return struct.unpack_from('<I',b,o)[0]
def raw_inflate(data,s,cs,osz):
    return zlib.decompressobj(-15).decompress(data[s+2:s+cs])[:osz]

def readpath(fl,c,end):
    pb=fl[c:end]; z=pb.find(b'\x00')
    return pb[:z].decode('latin-1') if z>=0 else pb.decode('latin-1')

def parse40(fl,c):
    ns=c; size=u32(fl,c); c+=4; typ=u16(fl,c); c+=2
    n={'nodeStart':ns,'nodeSize':size,'type':typ,'unc':0,'off':0,'comp':0,'index':0,'path':''}
    if typ==0:
        c+=3
        b=fl[c]; c+=1
        if b==0xE2:
            c+=30
        else:
            c+=14
            unc=u32(fl,c); c+=4
            off=u32(fl,c); c+=4
            comp=u32(fl,c); c+=4
            c+=4
            c+=24  # times
            n.update(unc=unc,off=off,comp=comp)
        n['path']=readpath(fl,c,ns+size)
    return n

def main(installer,outdir,mode):
    data=open(installer,'rb').read(); flen=len(data)
    pos=data.find(SIG)+6; dbs=-1; fl=None
    while pos+64<=flen:
        bid=u16(data,pos); pos+=4; bs=u32(data,pos); pos+=4; nxt=pos+bs
        if bid==FILE_DATA: dbs=pos
        elif bid==FILE_LIST:
            dec=u32(data,pos); assert data[pos+4]==1
            fl=raw_inflate(data,pos+5,bs-5,dec)
        pos=nxt
    fileNumber=u16(fl,0); c=4
    files=[]
    for i in range(fileNumber):
        n=parse40(fl,c)
        c=n['nodeStart']+n['nodeSize']
        if n['type']==0: files.append(n)
    if mode=='paths':
        for i,n in enumerate(files):
            print("%d/%d\t%s"%(i+1,len(files),n['path']))
        return 0
    os.makedirs(outdir,exist_ok=True); ok=0; fail=0
    for n in files:
        dest=os.path.join(outdir,n['path'].replace('\\','/'))
        d=os.path.dirname(dest)
        if d: os.makedirs(d,exist_ok=True)
        if n['unc']==0:
            open(dest,'wb').close(); ok+=1; continue
        base=dbs+n['off']+4
        m=data[base]
        if m==1:
            # raw deflate is self-terminating: feed the rest (zero-copy) and stop at EOF
            d=zlib.decompressobj(-15)
            content=d.decompress(memoryview(data)[base+3:])[:n['unc']]
        elif m==0:
            content=data[base+1:base+1+n['comp']-7]
        elif m==2:
            # BZ2: bzip2 stream starts right after the method byte (no skip)
            content=bz2.BZ2Decompressor().decompress(memoryview(data)[base+1:])[:n['unc']]
        else: fail+=1; print("UNSUP",m,n['path']); continue
        if len(content)!=n['unc']: fail+=1; print("size",n['path'],len(content),n['unc']); continue
        open(dest,'wb').write(content); ok+=1
    print("version 40, files=%d ok=%d fail=%d"%(len(files),ok,fail))
    return 1 if fail else 0

if __name__=='__main__':
    sys.exit(main(sys.argv[1],sys.argv[2],sys.argv[3] if len(sys.argv)>3 else 'extract'))
