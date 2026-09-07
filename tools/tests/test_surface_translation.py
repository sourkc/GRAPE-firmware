"""Differential cache/setter checks with extracted production C, Windows LLVM.

Minimal structs and deterministic math stand-ins isolate cache equivalence and
damage-call ordering; this is not a device/libm or pixel-rendering simulation.
"""
import argparse
import ctypes as ct
from pathlib import Path
import random
import subprocess
from test_renderer_fastpaths import function

PRELUDE=r'''
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned long long size_t;
typedef _Bool bool;
typedef int esp_err_t;
#define true 1
#define false 0
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define FLT_EPSILON 0.00000011920928955078125f
#define GRAPE_TIME_SCOPE(x)
#define GRAPE_ROTATION_BACKEND_THREE_SHEAR 1
#define GRAPE_SURFACE_AA_COVERAGE_4X 1
#define isfinite(x) __builtin_isfinite(x)
#define fabsf(x) __builtin_fabsf(x)
#define fminf(a,b) ((a)<(b)?(a):(b))
#define fmaxf(a,b) ((a)>(b)?(a):(b))
int _fltused=0;
void *memcpy(void *d,const void *s,size_t n){for(size_t i=0;i<n;++i)((char*)d)[i]=((const char*)s)[i];return d;}
void *memset(void *d,int v,size_t n){for(size_t i=0;i<n;++i)((char*)d)[i]=(char)v;return d;}
int memcmp(const void *a,const void *b,size_t n){for(size_t i=0;i<n;++i)if(((const unsigned char*)a)[i]!=((const unsigned char*)b)[i])return 1;return 0;}
static float cosf(float x){return .75f+x*.013f;}
static float sinf(float x){return x*.017f;}
static float atan2f(float y,float x){return y/x;}
static float tanf(float x){return x*.23f;}
static float floorf(float x){int i=(int)x;return (float)(i-(x<(float)i));}
static float ceilf(float x){int i=(int)x;return (float)(i+(x>(float)i));}
typedef struct {float x,y,scale_x,scale_y,rotation,origin_x,origin_y;} grape_transform_t;
typedef struct {int32_t x,y,width,height;} grape_rect_t;
typedef struct {int rotation_backend;} grape_context_t;
typedef struct {
 grape_context_t *context; grape_transform_t transform;
 uint32_t width,height; bool visible; int aa;
 float cos_rotation,sin_rotation,normalized_rotation,shear_x_coefficient;
 bool shear_cache_valid;
 float local_x_from_screen_x,local_x_from_screen_y,local_x_offset;
 float local_y_from_screen_x,local_y_from_screen_y,local_y_offset;
 float aa_local_dx[4],aa_local_dy[4];
 float aa_local_min_dx,aa_local_max_dx,aa_local_min_dy,aa_local_max_dy;
 grape_rect_t bounds;
} grape_surface_t;
static int full_calls,damage_calls,fail_call;
static grape_rect_t damage_bounds[2];
static void surface_recache_texture_mapping(grape_surface_t *s){(void)s;}
static esp_err_t mark_surface_coverage(grape_surface_t *s){
 if(damage_calls<2)damage_bounds[damage_calls]=s->bounds;
 ++damage_calls;return damage_calls==fail_call?2:0;
}
'''
WRAPPER=r'''
__declspec(dllexport) int check(float *a,float *b,unsigned width,unsigned height,int aa,int visible,int backend,int fail,int expected_full){
 grape_context_t context={backend};grape_surface_t original={0},fast,reference;
 original.context=&context;memcpy(&original.transform,a,28);
 original.width=width;original.height=height;original.aa=aa;original.visible=visible;
 grape_surface_recache(&original);fast=original;reference=original;
 grape_transform_t next;memcpy(&next,b,28);
 fail_call=fail;damage_calls=0;full_calls=0;
 int ret=grape_surface_set_transform(&fast,&next),calls=damage_calls,full=full_calls;
 grape_rect_t bounds[2];memcpy(bounds,damage_bounds,sizeof(bounds));
 damage_calls=0;full_calls=0;
 int refret=reference_set_transform(&reference,&next);
 if(ret!=refret)return 1;
 if(memcmp(&fast,&reference,sizeof(fast)))return 2;
 if(calls!=damage_calls||memcmp(bounds,damage_bounds,calls*sizeof(grape_rect_t)))return 3;
 if(expected_full>=0&&full!=expected_full)return 4;
 return 0;
}
'''

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--clang',required=True)
    parser.add_argument('--lld',required=True)
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[2]
    source=(root/'components/grape/src/grape_surface.c').read_text()
    pieces=[function(source,n) for n in ['transform_point','grape_surface_calculate_bounds','surface_recache_transform','grape_surface_recache','surface_transform_finite','surface_recache_translation','grape_surface_set_transform']]
    pieces[2]=pieces[2].replace('{','{ ++full_calls;',1)
    frozen=(Path(__file__).parent/'fixtures/surface_set_transform_before.c').read_text().replace('grape_surface_set_transform','reference_set_transform')
    build=root/'build-host-translation';build.mkdir(exist_ok=True)
    (build/'test.c').write_text(PRELUDE+'\n'.join(pieces)+frozen+WRAPPER)
    subprocess.run([args.clang,'--target=x86_64-pc-windows-msvc','-O2','-ffreestanding','-fno-builtin','-ffp-contract=off','-c',str(build/'test.c'),'-o',str(build/'test.obj')],check=True)
    subprocess.run([args.lld,'-flavor','link','/dll','/noentry','/machine:x64','/out:'+str(build/'test.dll'),str(build/'test.obj')],check=True)
    dll=ct.CDLL(str(build/'test.dll'));fn=dll.check
    fn.argtypes=[ct.POINTER(ct.c_float),ct.POINTER(ct.c_float),ct.c_uint,ct.c_uint]+[ct.c_int]*5
    fn.restype=ct.c_int;rng=random.Random(9137);count=0
    for i in range(12000):
        a=[rng.uniform(-1000,1000),rng.uniform(-1000,1000),rng.choice([-.5,1,2,3]),rng.choice([-2,.5,1]),rng.uniform(-3,3),rng.uniform(-100,100),rng.uniform(-100,100)]
        b=a.copy();b[0]=rng.uniform(-1000,1000);b[1]=rng.uniform(-1000,1000)
        expected=0
        if i%7==0:b[2+i%5]+=.25;expected=1
        if i%11==0:b=a.copy();expected=0
        if i%13==0:b[rng.randrange(7)]=float('nan');expected=0
        if i%17==0:b[rng.randrange(7)]=float('inf');expected=0
        if i%19==0:b[2]=0;expected=0
        fail=1 if i%23==0 else (2 if i%29==0 else 0)
        if fail==1:expected=0
        result=fn((ct.c_float*7)(*a),(ct.c_float*7)(*b),rng.randrange(1025),rng.randrange(1025),i%2,(i%5)!=0,i%2,fail,expected)
        assert result==0,(i,result,a,b,fail,expected)
        count+=1
    print(f'{count} setter/cache comparisons passed; bounds, damage ordering, invalid inputs, failures and fallback checked.')

if __name__=='__main__':main()
