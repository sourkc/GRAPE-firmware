"""Differential tests of production CSC coefficient generation, not peripheral emulation.

Compiles the actual C encoder with Windows LLVM, then compares its accepted
predicates to independent arbitrary-precision edge equations including top-left
bias. Requires --clang and --lld, like the existing renderer host checks.
"""
import argparse
import ctypes as ct
import json
from pathlib import Path
import random
import subprocess


class Coeff(ct.Structure):
    _fields_ = [(n, ct.c_int32) for n in ('a','b','c','d')]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--clang',required=True,type=Path)
    parser.add_argument('--lld',required=True,type=Path)
    parser.add_argument('--build-dir',type=Path,default=Path('build-ppa-edge-host'))
    args=parser.parse_args()
    build=args.build_dir.resolve()
    build.mkdir(parents=True,exist_ok=True)
    root=Path(__file__).resolve().parents[2]
    source=(root/'components/grape/src/grape_ppa_edge_math.c').read_text()
    source='\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
    source=source.replace('bool grape_ppa_edge_encode(', '__declspec(dllexport) bool grape_ppa_edge_encode(')
    prelude='''
typedef _Bool bool;
#define true 1
#define false 0
typedef long long int64_t;
typedef unsigned long long uint64_t;
typedef int int32_t;
typedef unsigned uint32_t;
#define INT64_MIN (-9223372036854775807LL-1)
typedef struct {int32_t a,b,c,d;} grape_ppa_edge_coeff_t;
'''
    (build/'edge.c').write_text(prelude+source)
    subprocess.run([str(args.clang),'--target=x86_64-pc-windows-msvc','-O2','-ffreestanding',
                    '-fno-builtin','-c',str(build/'edge.c'),'-o',str(build/'edge.obj')],check=True)
    subprocess.run([str(args.lld),'-flavor','link','/dll','/noentry','/machine:x64',
                    '/out:'+str(build/'edge.dll'),str(build/'edge.obj')],check=True)
    dll=ct.CDLL(str(build/'edge.dll'))
    encode=dll.grape_ppa_edge_encode
    encode.argtypes=[ct.c_int64,ct.c_int64,ct.c_int64,ct.c_uint32,ct.c_uint32,ct.c_int,ct.POINTER(Coeff)]
    encode.restype=ct.c_bool
    rng=random.Random(20260906)
    counts={'accepted_edges':0,'rejected_edges':0,'pixel_sign_comparisons':0,'triangles':0}

    def check(row,sx,sy,w,h,bias,required=None):
        out=Coeff(101,102,103,104)
        initial=bytes(out)
        ok=encode(row,sx,sy,w,h,bias,ct.byref(out))
        if required is not None: assert ok==required,(row,sx,sy,w,h,bias,ok)
        if not ok:
            assert bytes(out)==initial,'Rejection modified output'
            counts['rejected_edges']+=1
            return None
        counts['accepted_edges']+=1
        assert -512<=out.a<=511 and -1024<=out.b<=1023 and out.c==0
        assert -131072<=out.d<=131071
        mask=[]
        for y in range(h):
            for x in range(w):
                numerator=out.a*x+out.b*y+out.d+bias
                assert 0<=numerator<65536
                hardware_predicate=(numerator//256)>=128
                reference=row+sx*x+sy*y>=0
                assert hardware_predicate==reference,(row,sx,sy,w,h,bias,x,y,list(bytes(out)))
                mask.append(hardware_predicate)
        counts['pixel_sign_comparisons']+=w*h
        return mask

    # Zero and negative row remainders: -1 top-left ownership must survive gcd.
    for bias in (0,128):
        for row in (-1001,-257,-256,-255,-1,0,1,255,256):
            for sx,sy in ((256,512),(-256,512),(0,-256),(256,0),(0,0)):
                check(row,sx,sy,16,16,bias,True)
        for row,sx,sy,w,h in ((0,-2**63,1,16,16),(2**63-1,1,0,16,16),
                              (0,2**63-1,0,16,16),(0,1,1,0,16),(0,1,1,65,16)):
            check(row,sx,sy,w,h,bias,False)
        # Genuinely crossing edge outside the A-register range.
        check(-512,1025,1,16,16,bias,False)
    check(0,1,1,16,16,64,False)

    for _ in range(12000):
        sx,sy=rng.randint(-25000,25000),rng.randint(-25000,25000)
        factor=rng.choice((1,8,64,1000))
        sx*=factor; sy*=factor
        w,h=rng.randint(1,16),rng.randint(1,16)
        row=-sx*rng.randrange(w)-sy*rng.randrange(h)+rng.choice((-1,0,1,17))
        check(row,sx,sy,w,h,rng.choice((0,128)))

    # Larger grids preserve exact signed predicates and top-left remainders.
    for side in (32,64):
        for _ in range(500):
            sx,sy=rng.randint(-127,127),rng.randint(-127,127)
            g=rng.choice((1,8,1024))
            row=(-sx*rng.randrange(side)-sy*rng.randrange(side))*g+rng.choice((-1,0,1))
            check(row,sx*g,sy*g,side,side,rng.choice((0,128)),True)

    # Independent triangle winding, screen subpixels, and top-left convention.
    for _ in range(3000):
        vertices=[(rng.randrange(-64,193),rng.randrange(-64,193)) for _ in range(3)]
        area=(vertices[1][0]-vertices[0][0])*(vertices[2][1]-vertices[0][1])-\
             (vertices[1][1]-vertices[0][1])*(vertices[2][0]-vertices[0][0])
        if not area: continue
        if area<0: vertices[1],vertices[2]=vertices[2],vertices[1]
        bias=rng.choice((0,128))
        masks=[]
        for i,a in enumerate(vertices):
            b=vertices[(i+1)%3]
            dx,dy=b[0]-a[0],b[1]-a[1]
            top_left=dy<0 or (dy==0 and dx>0)
            row=dx*(4-a[1])-dy*(4-a[0])+(0 if top_left else -1)
            masks.append(check(row,-dy*8,dx*8,16,16,bias,True))
        combined=[all(mask[i] for mask in masks) for i in range(256)]
        assert len(combined)==256
        counts['triangles']+=1
    assert counts['rejected_edges']>100 and counts['accepted_edges']>8000
    (build/'results.json').write_text(json.dumps(counts,indent=2)+'\n')
    print(json.dumps(counts,indent=2))


if __name__=='__main__': main()
