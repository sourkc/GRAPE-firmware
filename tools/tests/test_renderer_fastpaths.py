"""Host differential checks of extracted production C functions (Windows clang/lld).

The small stand-in structs model only fields used by these functions. Firmware
compilation validates integration with real headers; this checks algorithm output.
"""
import argparse
import ctypes as ct
import json
from pathlib import Path
import random
import re
import subprocess


def function(text, name):
    start = None
    for match in re.finditer(r'\b' + re.escape(name) + r'\s*\(', text):
        paren = text.index('(', match.start())
        depth, end = 1, paren + 1
        while depth:
            depth += (text[end] == '(') - (text[end] == ')')
            end += 1
        if text[end:].lstrip().startswith('{'):
            start = match.start()
            break
    if start is None: raise ValueError('No definition for ' + name)
    start = text.rfind('\n', 0, start) + 1
    # Include a split declaration line when present.
    if text[start:].startswith(name): start = text.rfind('\n', 0, start - 1) + 1
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


PRELUDE = r'''
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned long long size_t;
typedef _Bool bool;
typedef int esp_err_t;
typedef int grape_pixel_format_t;
#define true 1
#define false 0
#define NULL ((void*)0)
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define GRAPE_PIXEL_FORMAT_RGB565 0
#define GRAPE_PIXEL_FORMAT_RGB888 1
#define GRAPE_PIXEL_FORMAT_A8 2
#define GRAPE_PIXEL_FORMAT_RGBA8888 3
#define GRAPE_TEXTURE_FILTER_NEAREST 0
#define GRAPE_SURFACE_AA_NONE 0
#define CONFIG_GRAPE_TEXTURE_OCCUPANCY_CELL_SIZE 8
#define isfinite(x) __builtin_isfinite(x)
#define fabsf(x) __builtin_fabsf(x)
int _fltused = 0;
void *memcpy(void *d, const void *s, size_t n) {
    for (size_t i=0;i<n;i++) ((uint8_t*)d)[i]=((const uint8_t*)s)[i]; return d;
}
void *memset(void *d, int b, size_t n) {
    for (size_t i=0;i<n;i++) ((uint8_t*)d)[i]=(uint8_t)b; return d;
}
typedef struct { uint8_t r,g,b,a; } grape_color_t;
typedef grape_color_t rgba8_t;
typedef struct { int32_t x,y,width,height; } grape_rect_t;
typedef struct {
    uint8_t *pixels; size_t stride; uint32_t width,height; int format;
    uint8_t *occupancy; size_t occupancy_bitmap_size; uint32_t occupancy_columns,occupancy_rows;
    size_t occupancy_occupied_count; bool occupancy_all_empty,occupancy_all_full;
} grape_texture_t;
typedef struct { void *pixels; size_t stride; } target_t;
typedef struct { int format; } display_t;
typedef struct { target_t render_target; display_t display_info; } grape_context_t;
typedef struct {
    grape_texture_t *texture;
    float local_x_from_screen_x,local_x_from_screen_y,local_x_offset;
    float local_y_from_screen_x,local_y_from_screen_y,local_y_offset;
    grape_color_t tint; uint8_t opacity; bool texture_mapping_identity;
    size_t shader_count; int texture_filter,aa;
} grape_surface_t;
'''
WRAPPERS = r'''
#define API __declspec(dllexport)
API int test_wrap(int value, int limit) { return gpu_wrap_index(value,limit); }
API uint32_t test_mix(uint32_t *colors, uint32_t fx, uint32_t fy, int fast) {
    grape_color_t c[4]; memcpy(c,colors,16);
    uint32_t ix=256-fx,iy=256-fy;
    grape_color_t result = fast ? gpu_mix_opaque_bilinear(c[0],c[1],c[2],c[3],ix*iy,fx*iy,ix*fy,fx*fy)
        : gpu_mix_bilinear(c[0],c[1],c[2],c[3],ix*iy,fx*iy,ix*fy,fx*fy);
    return result.r | ((uint32_t)result.g<<8) | ((uint32_t)result.b<<16) | ((uint32_t)result.a<<24);
}
API int test_full(grape_texture_t *t) { return grape_texture_rebuild_occupancy(t); }
API void test_partial(grape_texture_t *t,uint32_t x,uint32_t y,uint32_t w,uint32_t h) {
    texture_update_occupancy_rect(t,x,y,w,h);
}
API int test_copy(grape_context_t *c, grape_surface_t *s, grape_rect_t *r,int fast) {
    if (fast) return raster_rgb565_copy(c,s,*r,2);
    raster_surface_rgb565(c,s,*r,*r,2); return 1;
}
'''


class Color(ct.Structure): _fields_ = [(k, ct.c_uint8) for k in 'rgba']
class Rect(ct.Structure): _fields_ = [(k, ct.c_int32) for k in ('x','y','width','height')]
class Texture(ct.Structure):
    _fields_ = [('pixels',ct.POINTER(ct.c_uint8)),('stride',ct.c_size_t),('width',ct.c_uint32),
                ('height',ct.c_uint32),('format',ct.c_int),('occupancy',ct.POINTER(ct.c_uint8)),
                ('occupancy_bitmap_size',ct.c_size_t),('occupancy_columns',ct.c_uint32),
                ('occupancy_rows',ct.c_uint32),('occupancy_occupied_count',ct.c_size_t),
                ('occupancy_all_empty',ct.c_bool),('occupancy_all_full',ct.c_bool)]
class Target(ct.Structure): _fields_ = [('pixels',ct.c_void_p),('stride',ct.c_size_t)]
class Display(ct.Structure): _fields_ = [('format',ct.c_int)]
class Context(ct.Structure): _fields_ = [('render_target',Target),('display_info',Display)]
class Surface(ct.Structure):
    _fields_ = [('texture',ct.POINTER(Texture))] + [(k,ct.c_float) for k in (
        'local_x_from_screen_x','local_x_from_screen_y','local_x_offset',
        'local_y_from_screen_x','local_y_from_screen_y','local_y_offset')] + [
        ('tint',Color),('opacity',ct.c_uint8),('texture_mapping_identity',ct.c_bool),
        ('shader_count',ct.c_size_t),('texture_filter',ct.c_int),('aa',ct.c_int)]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--clang',required=True,type=Path)
    parser.add_argument('--lld',required=True,type=Path)
    parser.add_argument('--build-dir',type=Path,default=Path('build-host-tests'))
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[2]
    core=root/'components/grape/src'
    gpu=(core/'grape_gpu_texture.c').read_text()
    tex=(core/'grape_texture.c').read_text()
    comp=(core/'grape_compositor.c').read_text()
    reference=(Path(__file__).parent/'fixtures/renderer_reference.c').read_text()
    code=PRELUDE+'\n'+reference+'\n'
    for source,names in [(gpu,['gpu_wrap_index','gpu_mix_opaque_bilinear']),
                         (tex,['texture_update_occupancy_rect']),
                         (comp,['raster_rgb565_copy','raster_a8_rgb565_rows','raster_surface_a8'])]:
        for name in names: code+=function(source,name)+'\n'
    code+=(Path(__file__).parent/'fixtures/raster_a8_before.c').read_text()
    code+=WRAPPERS
    code+=r'''
API int test_a8(grape_context_t *c, grape_surface_t *s, grape_rect_t *r, int mode) {
        if (mode==1) return raster_a8_rgb565_rows(c,s,*r,2);
        if (mode==2) raster_surface_a8(c,s,*r,*r,2);
        else raster_surface_a8_before(c,s,*r,*r,2);
        return 0;
    }''' 
    build=args.build_dir.resolve();build.mkdir(parents=True,exist_ok=True)
    (build/'test.c').write_text(code)
    subprocess.run([str(args.clang),'--target=x86_64-pc-windows-msvc','-O2','-ffreestanding',
        '-fno-builtin','-ffp-contract=off','-c',str(build/'test.c'),'-o',str(build/'test.obj')],check=True)
    subprocess.run([str(args.lld),'-flavor','link','/dll','/noentry','/machine:x64',
        '/out:'+str(build/'test.dll'),str(build/'test.obj')],check=True)
    dll=ct.CDLL(str(build/'test.dll'))
    dll.test_mix.restype=ct.c_uint32
    rng=random.Random(0x47524150)
    counts={}
    n=0
    for limit in [1<<i for i in range(31)]+[3,7,31,63,65,1000,2147483647]:
        for value in [-2147483648,-1,0,1,2147483647]+[rng.randint(-2147483648,2147483647) for _ in range(500)]:
            assert dll.test_wrap(value,limit)==value%limit,(value,limit)
            n+=1
    counts['wrap_cases']=n
    colors=(ct.c_uint32*4)(0xff012345,0xffabcdef,0xff00ffff,0xffff0000)
    for fx in range(257):
        for fy in range(257):
            assert dll.test_mix(colors,fx,fy,0)==dll.test_mix(colors,fx,fy,1),(fx,fy)
    for _ in range(50000):
        colors=(ct.c_uint32*4)(*[rng.getrandbits(24)|0xff000000 for _ in range(4)])
        fx,fy=rng.randrange(257),rng.randrange(257)
        assert dll.test_mix(colors,fx,fy,0)==dll.test_mix(colors,fx,fy,1)
    counts['opaque_bilinear_cases']=257*257+50000
    n=0
    for fmt in (2,3):
        for w,h in [(1,1),(7,9),(8,8),(9,17),(31,23),(65,33)]:
            bpp=1 if fmt==2 else 4; stride=w*bpp+8
            pixels=(ct.c_uint8*(stride*h))()
            cols,rows=(w+7)//8,(h+7)//8; size=(cols*rows+7)//8
            a,b=(ct.c_uint8*size)(),(ct.c_uint8*size)()
            ta=Texture(pixels,stride,w,h,fmt,a,size,cols,rows,0,True,False)
            tb=Texture(pixels,stride,w,h,fmt,b,size,cols,rows,0,True,False)
            dll.test_full(ct.byref(ta));dll.test_full(ct.byref(tb))
            for _ in range(400):
                x,y=rng.randrange(w),rng.randrange(h)
                rw,rh=rng.randint(1,w-x),rng.randint(1,h-y)
                for py in range(y,y+rh):
                    for px in range(x,x+rw): pixels[py*stride+px*bpp+bpp-1]=rng.choice([0,0,0,1,255])
                dll.test_partial(ct.byref(ta),x,y,rw,rh); dll.test_full(ct.byref(tb))
                assert bytes(a)==bytes(b),(fmt,w,h,x,y,rw,rh)
                assert (ta.occupancy_occupied_count,ta.occupancy_all_empty,ta.occupancy_all_full)==(
                    tb.occupancy_occupied_count,tb.occupancy_all_empty,tb.occupancy_all_full)
                n+=1
    counts['incremental_occupancy_updates']=n
    accepted=rejected=0
    for i in range(4000):
        stride=64*2+8; pixels=(ct.c_uint8*(stride*64))(*[rng.randrange(256) for _ in range(stride*64)])
        texture=Texture(pixels,stride,64,64,0,None,0,0,0,0,False,True)
        surface=Surface(ct.pointer(texture),rng.choice([1,.5]),0,rng.randint(-8,8)/2,
            0,rng.choice([1,.5]),rng.randint(-8,8)/2,Color(255,255,255,255),255,True,0,0,0)
        if i%11==0: surface.shader_count=1
        if i%13==0: surface.local_x_offset=1.234
        if i%17==0: surface.local_x_from_screen_y=0.00001
        if i%19==0: surface.tint.a=254
        target_stride=128+16
        a=(ct.c_uint8*(target_stride*64))(*([0xa5]*(target_stride*64)))
        b=(ct.c_uint8*len(a)).from_buffer_copy(a)
        ca=Context(Target(ct.addressof(a),target_stride),Display(0))
        cb=Context(Target(ct.addressof(b),target_stride),Display(0))
        x,y=rng.randrange(16),rng.randrange(16)
        rect=Rect(x,y,rng.randint(1,64-x),rng.randint(1,64-y))
        used=dll.test_copy(ct.byref(ca),ct.byref(surface),ct.byref(rect),1)
        if used:
            dll.test_copy(ct.byref(cb),ct.byref(surface),ct.byref(rect),0)
            assert bytes(a)==bytes(b),(i,x,y,rect.width,rect.height)
            accepted+=1
        else:
            assert bytes(a)==bytes(b),'Rejected fast path modified output'
            rejected+=1
    assert accepted>1000 and rejected>1000
    counts['rgb565_accepted_comparisons']=accepted
    counts['rgb565_rejected_untouched']=rejected
    accepted=0
    for i in range(3000):
        stride=20
        pixels=(ct.c_uint8*(stride*16))(*[rng.choice([0,255,rng.randrange(256)]) for _ in range(stride*16)])
        texture=Texture(pixels,stride,16,16,2,None,0,0,0,0,False,True)
        surface=Surface(ct.pointer(texture),rng.choice([1,.5,2,-1,1.25]),0,rng.uniform(-4,20),
            0,rng.choice([1,.5,2,-1]),rng.uniform(-4,20),Color(235,230,255,255),255,True,0,0,0)
        if i%7==0: surface.opacity=128
        if i%11==0: surface.tint.a=128
        if i%13==0: surface.local_y_from_screen_x=.25
        if i%17==0: surface.texture_mapping_identity=False
        if i%19==0: surface.shader_count=1
        if i%23==0: surface.texture_filter=1
        if i%29==0: surface.aa=1
        a=(ct.c_uint8*(72*32))(*[rng.randrange(256) for _ in range(72*32)])
        b=(ct.c_uint8*len(a)).from_buffer_copy(a)
        c=(ct.c_uint8*len(a)).from_buffer_copy(a)
        ca=Context(Target(ct.addressof(a),72),Display(0))
        cb=Context(Target(ct.addressof(b),72),Display(0))
        cc=Context(Target(ct.addressof(c),72),Display(0))
        x,y=rng.randrange(8),rng.randrange(8)
        rect=Rect(x,y,rng.randint(1,32-x),rng.randint(1,32-y))
        initial=bytes(a)
        used=dll.test_a8(ct.byref(ca),ct.byref(surface),ct.byref(rect),1)
        dll.test_a8(ct.byref(cb),ct.byref(surface),ct.byref(rect),0)
        dll.test_a8(ct.byref(cc),ct.byref(surface),ct.byref(rect),2)
        assert bytes(b)==bytes(c),('A8 dispatch',i)
        if used:
            assert bytes(a)==bytes(b),('A8 fast',i)
            accepted+=1
        else:
            assert bytes(a)==initial,('A8 rejection modified output',i)
    assert 1000<accepted<3000
    counts['a8_dispatch_comparisons']=3000
    counts['a8_accepted_comparisons']=accepted
    print(json.dumps(counts,indent=2))
    (build/'results.json').write_text(json.dumps(counts,indent=2)+'\n')


if __name__=='__main__': main()
