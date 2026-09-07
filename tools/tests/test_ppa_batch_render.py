"""Exercise production batch dispatch/commit with a software CSC/PPA stand-in."""
import argparse
import ctypes as ct
from pathlib import Path
import subprocess
from test_renderer_fastpaths import function

PRELUDE = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#define CONFIG_GRAPE_PPA_TRIANGLE_VALIDATE 1
#define EDGE_SIDE 16U
#define EDGE_MAX_SIDE 64U
#define ESP_OK 0
#define ESP_ERR_INVALID_RESPONSE 1
#define GRAPE_GPU_SAMPLE_COUNT_1 1
#define GRAPE_GPU_FRAGMENT_PROGRAM_SOLID_COLOR 0
#define GRAPE_GPU_FRAGMENT_PROGRAM_PUSH_COLOR 1
#define GRAPE_FEATURE_PPA_TRIANGLE_EDGES 0
#define GRAPE_FEATURE_PPA_TRIANGLE_VALIDATE 1
#define GRAPE_FEATURE_PPA_TRIANGLE_BATCH 2
#define GRAPE_FEATURE_PPA_TRIANGLE_OVERLAP 3
#define assert(x) do {if(!(x))__builtin_trap();}while(0)
typedef int esp_err_t;
typedef struct {int32_t a,b,c,d;} grape_ppa_edge_coeff_t;
typedef struct {uint8_t r,g,b,a;} grape_color_t;
typedef struct {int x,y,width,height;} grape_rect_t;
typedef struct { int min_x,min_y,max_x,max_y;
    int64_t row_e0,row_e1,row_e2,e0_step_x,e1_step_x,e2_step_x,e0_step_y,e1_step_y,e2_step_y;
} grape_gpu_triangle_setup_t;
typedef struct { struct {bool test_enable,write_enable;} depth; int fragment_program;
} grape_gpu_pipeline_desc_t;
typedef struct {grape_gpu_pipeline_desc_t desc;} pipeline_t;
typedef struct { uint64_t triangles_seen,triangles_completed,tiles_computed,large_tiles_computed,
    pixels_computed,cpu_tiles_computed,cpu_pixels_computed,overlap_triangles,prepare_us,driver_us,wait_us,cpu_render_us,validation_us,commit_us,fallback_unsupported,fallback_coefficients,pixels_validated,
    validation_mismatches,compute_us; unsigned errors;
} grape_gpu_ppa_triangle_stats_t;
typedef struct {bool (*work)(void*);void *work_arg;uint64_t driver_us,wait_us;unsigned side;int round_bias;grape_ppa_edge_coeff_t coeff[3];
    uint8_t coords[12288],edges[12288],mask[12288];} edge_compute_t;
typedef struct {uint8_t *pixels;size_t stride;} texture_t;
typedef texture_t grape_texture_t;
typedef struct {bool features[4];} grape_context_t;
typedef struct {grape_context_t *grape;pipeline_t *bound_pipeline;int sample_count;
    grape_gpu_ppa_triangle_stats_t ppa_triangle_stats;edge_compute_t *ppa_triangle_state;
    texture_t *color_attachment;} grape_gpu_context_t;
static unsigned dirty,corrupt;
static bool grape_feature_is_active(grape_context_t *g,int id){return g->features[id];}
static esp_err_t edge_prepare(grape_gpu_context_t *g){(void)g;return 0;}
static void edge_fail(grape_gpu_context_t *g,int err){(void)err;++g->ppa_triangle_stats.errors;}
static int64_t esp_timer_get_time(void){return 0;}
static void grape_gpu_dirty_add(grape_gpu_context_t *g,grape_rect_t r){(void)g;(void)r;++dirty;}
static int edge_csc(edge_compute_t *s){
    for(unsigned i=0;i<s->side*s->side;++i) for(unsigned lane=0;lane<3;++lane){
        grape_ppa_edge_coeff_t c=s->coeff[lane];
        int n=c.a*s->coords[i*3+2]+c.b*s->coords[i*3+1]+c.d+128;
        s->edges[i*3+2-lane]=n/256;
    }
    if(s->work)for(unsigned i=0;i<2;++i)s->work(s->work_arg);
    return 0;
}
static int edge_key(edge_compute_t *s){
    for(unsigned i=0;i<s->side*s->side;++i){
        bool inside=s->edges[i*3]>=128 && s->edges[i*3+1]>=128 && s->edges[i*3+2]>=128;
        for(unsigned j=0;j<3;++j)s->mask[i*3+j]=inside?255:0;
    }
    if(s->work)for(unsigned i=0;i<3;++i)s->work(s->work_arg);
    if(corrupt==2)return ESP_ERR_INVALID_RESPONSE;
    if(corrupt)s->mask[0]^=255;
    return 0;
}
void *memset(void *p,int v,size_t n){for(size_t i=0;i<n;++i)((uint8_t*)p)[i]=v;return p;}
'''
CHECKS = r'''
__declspec(dllexport) int run_checks(void){
    static edge_compute_t state;
    static uint8_t pixels[112*112*4];
    const grape_color_t color={17,35,97,255};
    for(unsigned overlap=0;overlap<2;++overlap)
    for(unsigned batch=0;batch<2;++batch)for(unsigned validate=0;validate<2;++validate)
    for(unsigned kind=0;kind<3;++kind){
        memset(&state,0,sizeof(state));memset(pixels,73,sizeof(pixels));dirty=corrupt=0;
        state.round_bias=128;state.side=16;edge_coordinates(&state);
        grape_context_t grape={{true,validate,batch,overlap}};
        pipeline_t pipeline={0};texture_t target={pixels,112*4};
        grape_gpu_context_t gpu={.grape=&grape,.bound_pipeline=&pipeline,.sample_count=1,
            .ppa_triangle_state=&state,.color_attachment=&target};
        grape_gpu_triangle_setup_t t={.min_x=3,.min_y=5,.max_x=102,.max_y=100,
            .row_e0=-15,.row_e1=-20,.row_e2=130,
            .e0_step_x=1,.e1_step_y=1,.e2_step_x=-1,.e2_step_y=-1};
        if(kind==1){t.row_e0=-10000;t.e0_step_x=511;t.e0_step_y=1023;}
        if(kind==2){t.max_x=27;t.max_y=26;}
        if(!grape_gpu_ppa_try_triangle(&gpu,&t,color))return 1;
        if(gpu.ppa_triangle_stats.errors || gpu.ppa_triangle_stats.validation_mismatches)return 2;
        if((gpu.ppa_triangle_stats.pixels_validated!=0)!=validate)return 3;
        if(batch && kind==0 && !gpu.ppa_triangle_stats.large_tiles_computed)return 4;
        if(!batch && gpu.ppa_triangle_stats.large_tiles_computed)return 5;
        if(kind==1 && state.side!=16)return 6; /* Wide coefficients shrink blocks. */
        for(int y=0;y<112;++y)for(int x=0;x<112;++x){
            int64_t u=x-t.min_x,v=y-t.min_y;
            bool in=x>=t.min_x && x<=t.max_x && y>=t.min_y && y<=t.max_y &&
                t.row_e0+t.e0_step_x*u+t.e0_step_y*v>=0 &&
                t.row_e1+t.e1_step_x*u+t.e1_step_y*v>=0 &&
                t.row_e2+t.e2_step_x*u+t.e2_step_y*v>=0;
            for(unsigned lane=0;lane<4;++lane)
                if(pixels[(y*112+x)*4+lane]!=(in?((const uint8_t*)&color)[lane]:73))return 7;
        }
        if(state.work || state.work_arg)return 10;
        if(overlap && kind==0 && (!gpu.ppa_triangle_stats.cpu_tiles_computed || !gpu.ppa_triangle_stats.overlap_triangles))return 11;
        if(!overlap && gpu.ppa_triangle_stats.cpu_tiles_computed)return 12;
        if(validate && !overlap){
            memset(pixels,73,sizeof(pixels));corrupt=1;
            if(grape_gpu_ppa_try_triangle(&gpu,&t,color) || !gpu.ppa_triangle_stats.validation_mismatches)return 8;
            for(unsigned i=0;i<sizeof(pixels);++i)if(pixels[i]!=73)return 9;
        }
        if(overlap && kind==0){
            /* A key-stage error after CPU work must clear transient pointers,
             * then the original renderer can recover the complete triangle. */
            memset(pixels,73,sizeof(pixels));corrupt=2;
            if(grape_gpu_ppa_try_triangle(&gpu,&t,color) || state.work || state.work_arg)return 13;
            grape_gpu_raster_color_block(&gpu,&t,color);
            for(int y=0;y<112;++y)for(int x=0;x<112;++x){
                int64_t u=x-t.min_x,v=y-t.min_y;
                bool in=x>=t.min_x && x<=t.max_x && y>=t.min_y && y<=t.max_y &&
                    t.row_e0+t.e0_step_x*u+t.e0_step_y*v>=0 &&
                    t.row_e1+t.e1_step_x*u+t.e1_step_y*v>=0 &&
                    t.row_e2+t.e2_step_x*u+t.e2_step_y*v>=0;
                for(unsigned lane=0;lane<4;++lane)
                    if(pixels[(y*112+x)*4+lane]!=(in?((const uint8_t*)&color)[lane]:73))return 14;
            }
            /* Another overlapping triangle must observe finished prior work.
             * Compare the mixed render directly with ordered CPU rendering. */
            static uint8_t expected[sizeof(pixels)];
            for(unsigned i=0;i<sizeof(pixels);++i)expected[i]=pixels[i];
            texture_t reference={expected,112*4};
            t.row_e0-=8;t.row_e1+=5;
            const grape_color_t next={213,45,21,255};
            gpu.color_attachment=&reference;
            grape_gpu_raster_color_block(&gpu,&t,next);
            gpu.color_attachment=&target;corrupt=0;
            if(!grape_gpu_ppa_try_triangle(&gpu,&t,next) || state.work || state.work_arg)return 15;
            for(unsigned i=0;i<sizeof(pixels);++i)if(pixels[i]!=expected[i])return 16;
        }
    }
    return 0;
}
'''


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--clang',required=True,type=Path)
    p.add_argument('--lld',required=True,type=Path)
    p.add_argument('--build-dir',type=Path,default=Path('build-ppa-batch-host'))
    a=p.parse_args(); root=Path(__file__).resolve().parents[2]
    backend=(root/'components/grape/src/grape_gpu_ppa.c').read_text()
    math=(root/'components/grape/src/grape_ppa_edge_math.c').read_text()
    math='\n'.join(line for line in math.splitlines() if not line.startswith('#include'))
    raster=(root/'components/grape/src/grape_gpu_raster.c').read_text()
    work_type=backend[backend.index('typedef struct {\n    grape_gpu_context_t *context;'):backend.index('static bool edge_claim(')]
    code=PRELUDE+math+'\n'+function(raster,'gpu_rasterize_color_only')+function(raster,'grape_gpu_raster_color_block')
    code+=function(backend,'edge_coordinates')+function(backend,'edge_tile_coeff')+work_type
    code+=''.join(function(backend,name) for name in ('edge_claim','edge_cpu_step','grape_gpu_ppa_try_triangle'))+CHECKS
    out=a.build_dir.resolve();out.mkdir(parents=True,exist_ok=True)
    (out/'batch.c').write_text(code)
    subprocess.run([str(a.clang),'--target=x86_64-pc-windows-msvc','-O2','-ffreestanding',
        '-fno-builtin','-c',str(out/'batch.c'),'-o',str(out/'batch.obj')],check=True)
    subprocess.run([str(a.lld),'-flavor','link','/dll','/noentry','/machine:x64',
        '/out:'+str(out/'batch.dll'),str(out/'batch.obj')],check=True)
    ret=ct.CDLL(str(out/'batch.dll')).run_checks();assert ret==0,ret
    print('Batch dispatch: 24 mode/geometry cases, padded bounds, shrinking blocks, validation-off counts and corrupt-mask rejection passed')


if __name__=='__main__':main()
