# Checks the Studio animation editor (test_studio_motion_edit.cpp): python3 test_studio_motion_edit.py move.dat edit.dat speed.dat
# Only entry 2 changes; chest (part 3) gets 0/+0.5/-0.25 rad over frames 5..15 with a 4-frame edge; the rest keeps its curves.
import struct,sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'tools'))
from motion import fcv
def body(p):
    d=open(p,'rb').read(); n=struct.unpack_from('>I',d,0)[0]
    o=[struct.unpack_from('>I',d,16+i*4)[0] for i in range(n)]+[len(d)]
    return [d[o[i]:o[i+1]] for i in range(n)]
def val(a,fmt,f,mx):
    keys=[(fcv.to_float(fmt[0],k[0]),fcv.to_float(fmt[1],k[1]) if fmt[1] else 0,fcv.to_float(fmt[1],k[2]) if fmt[1] else 0) for k in a.keys]
    fr=a.frames
    if f>=mx or f>=fr[-1]: return keys[-1][0]
    if f<=fr[0]: return keys[0][0]
    for i in range(len(fr)-1):
        if f==fr[i]: return keys[i][0]
        if fr[i]<f<fr[i+1]:
            t=(f-fr[i])/(fr[i+1]-fr[i]); t2=t*t;t3=t2*t
            h01=-2*t3+3*t2;h11=t3-t2;h10=h11-t2+t;h00=1-h01
            return keys[i][0]*h00+keys[i+1][0]*h01+keys[i][2]*h10+keys[i+1][1]*h11
def w(f,f0,f1,fade):
    if f0<=f<=f1: return 1
    away=f0-f if f<f0 else f-f1
    if away>fade: return 0
    x=1-away/(fade+1); return x*x*(3-2*x)
a=body(sys.argv[1]); b=body(sys.argv[2])
bad=0
for i,(x,y) in enumerate(zip(a,b)):
    if i!=2 and x!=y: print('entry changed',i); bad+=1
ma=fcv.parse(a[2]); mb=fcv.parse(b[2])
err=0
for ja,jb in zip(ma.joints,mb.joints):
    fa=fcv.FCC_FMT[ja.fcc_type]; fb=fcv.FCC_FMT[jb.fcc_type]
    for ax in range(3):
        for f10 in range(0,ma.max_frame*10+1):
            f=f10/10
            exp=val(ja.axes[ax],fa,f,ma.max_frame)
            if ja.parts_no==3 and ja.kind==2: exp+= [0,0.5,-0.25][ax]*w(round(f),5,15,4) if f==int(f) else 0
            got=val(jb.axes[ax],fb,f,mb.max_frame)
            if f==int(f) or not(ja.parts_no==3 and ja.kind==2): 
                e=abs(got-exp); err=max(err,e)
                if e>1e-4: bad+=1; print('diff',ja.parts_no,ax,f,exp,got) if bad<5 else None
print('max err',err,'bad',bad)
s=body(sys.argv[3]); ms=fcv.parse(s[2]); 
print('speed frames',ms.max_frame, 'carry',fcv.parse(s[34]).max_frame, struct.unpack_from('>BBHHHHH',s[0],8+12))
