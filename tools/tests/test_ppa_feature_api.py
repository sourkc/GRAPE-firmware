"""Exercise the real feature registry/API with a minimal host context."""
import argparse
import ctypes as ct
from pathlib import Path
import subprocess


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--clang',required=True,type=Path)
    p.add_argument('--lld',required=True,type=Path)
    p.add_argument('--build-dir',type=Path,default=Path('build-ppa-feature-host'))
    args=p.parse_args()
    root=Path(__file__).resolve().parents[2]
    out=args.build_dir.resolve(); out.mkdir(parents=True,exist_ok=True)
    (out/'esp_err.h').write_text('''#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NOT_FOUND 2
#define ESP_ERR_NOT_SUPPORTED 3
''')
    (out/'grape_internal.h').write_text('''#pragma once
#include "grape/grape_feature.h"
enum {
#define GRAPE_FEATURE_ENTRY(symbol,id,name,mode,flags) GRAPE_FEATURE_SLOT_##symbol,
#include "grape/grape_feature_registry.def"
#undef GRAPE_FEATURE_ENTRY
GRAPE_FEATURE_SLOT_COUNT
};
typedef struct {
    grape_feature_mode_t mode;
    bool available,active;
    grape_feature_unavailable_reason_t unavailable_reason;
} grape_feature_state_t;
struct grape_context { grape_feature_state_t features[GRAPE_FEATURE_SLOT_COUNT]; };
''')
    code=(root/'components/grape/src/grape_feature.c').read_text()
    code+='''
__declspec(dllexport) int run_checks(void) {
    grape_context_t a,b;
    grape_feature_info_t info;
    const grape_feature_id_t id=GRAPE_FEATURE_PPA_TRIANGLE_EDGES;
    grape_feature_init(&a); grape_feature_init(&b);
    if(grape_feature_get_info(&a,id,&info)!=ESP_OK)return 1;
    if(info.default_mode!=GRAPE_FEATURE_MODE_DISABLED || info.active || info.available)return 2;
    if(!(info.flags&GRAPE_FEATURE_FLAG_EXPERIMENTAL) || !(info.flags&GRAPE_FEATURE_FLAG_HAS_FALLBACK))return 3;
    if(grape_feature_enable(&a,id)!=ESP_ERR_NOT_SUPPORTED)return 4;
    grape_feature_set_availability(&a,id,true,GRAPE_FEATURE_UNAVAILABLE_NONE);
    if(grape_feature_is_active(&a,id))return 5;
    if(grape_feature_enable(&a,id)!=ESP_OK || !grape_feature_is_active(&a,id))return 6;
    if(grape_feature_is_active(&b,id))return 7;
    if(grape_feature_disable(&a,id)!=ESP_OK || grape_feature_is_active(&a,id))return 8;
    if(grape_feature_set_mode(&a,id,GRAPE_FEATURE_MODE_AUTO)!=ESP_OK || !grape_feature_is_active(&a,id))return 9;
    if(grape_feature_reset(&a,id)!=ESP_OK || grape_feature_is_active(&a,id))return 10;
    grape_feature_enable(&a,id);
    grape_feature_set_availability(&a,id,false,GRAPE_FEATURE_UNAVAILABLE_INIT_FAILED);
    grape_feature_get_info(&a,id,&info);
    if(info.active || info.available || info.unavailable_reason!=GRAPE_FEATURE_UNAVAILABLE_INIT_FAILED)return 11;
    if(grape_feature_enable(&a,id)!=ESP_ERR_NOT_SUPPORTED)return 12;
    const grape_feature_id_t validate=GRAPE_FEATURE_PPA_TRIANGLE_VALIDATE;
    const grape_feature_id_t batch=GRAPE_FEATURE_PPA_TRIANGLE_BATCH;
    grape_feature_get_info(&a,validate,&info);
    if(info.default_mode!=GRAPE_FEATURE_MODE_AUTO)return 13;
    grape_feature_get_info(&a,batch,&info);
    if(info.default_mode!=GRAPE_FEATURE_MODE_DISABLED)return 14;
    grape_feature_set_availability(&a,validate,true,GRAPE_FEATURE_UNAVAILABLE_NONE);
    grape_feature_set_availability(&a,batch,true,GRAPE_FEATURE_UNAVAILABLE_NONE);
    if(!grape_feature_is_active(&a,validate) || grape_feature_is_active(&a,batch))return 15;
    grape_feature_disable(&a,validate); grape_feature_enable(&a,batch);
    if(grape_feature_is_active(&a,validate) || !grape_feature_is_active(&a,batch))return 16;
    grape_feature_reset(&a,validate); grape_feature_reset(&a,batch);
    if(!grape_feature_is_active(&a,validate) || grape_feature_is_active(&a,batch))return 17;
    if(grape_feature_is_active(&b,validate) || grape_feature_is_active(&b,batch))return 18;
    const grape_feature_id_t overlap=GRAPE_FEATURE_PPA_TRIANGLE_OVERLAP;
    grape_feature_get_info(&a,overlap,&info);
    if(info.default_mode!=GRAPE_FEATURE_MODE_DISABLED || info.active)return 19;
    grape_feature_set_availability(&a,overlap,true,GRAPE_FEATURE_UNAVAILABLE_NONE);
    grape_feature_enable(&a,overlap);
    if(!grape_feature_is_active(&a,overlap) || grape_feature_is_active(&b,overlap))return 20;
    grape_feature_reset(&a,overlap);
    if(grape_feature_is_active(&a,overlap))return 21;
    return 0;
}
'''
    (out/'feature.c').write_text(code)
    subprocess.run([str(args.clang),'--target=x86_64-pc-windows-msvc','-O2','-ffreestanding','-fno-builtin',
        '-I'+str(out),'-I'+str(root/'components/grape/include'),'-c',str(out/'feature.c'),'-o',str(out/'feature.obj')],check=True)
    subprocess.run([str(args.lld),'-flavor','link','/dll','/noentry','/machine:x64',
        '/out:'+str(out/'feature.dll'),str(out/'feature.obj')],check=True)
    dll=ct.CDLL(str(out/'feature.dll'))
    result=dll.run_checks()
    assert result==0,result
    print('Feature registry/API: 21 state, isolation, toggle and failure checks passed')


if __name__=='__main__': main()
