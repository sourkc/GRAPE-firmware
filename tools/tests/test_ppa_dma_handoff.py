"""Run production CSC submission against separate CPU/DMA memory views.

Checks descriptor publication, RX status invalidation, error propagation, and
timeout quarantine. This models cache ownership, not the P4 arithmetic hardware.
"""
import argparse
import ctypes as ct
from pathlib import Path
import subprocess
from test_renderer_fastpaths import function


PRELUDE = r'''
#include <stdatomic.h>
typedef unsigned char uint8_t;
typedef unsigned long long size_t;
typedef _Bool bool;
typedef int esp_err_t;
typedef unsigned TickType_t;
typedef int BaseType_t;
typedef void *dma2d_channel_handle_t;
typedef int dma2d_event_data_t;
#define true 1
#define false 0
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define CACHE_ERROR 4
#define ENQUEUE_ERROR 5
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGE(...) ((void)0)
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 1
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C 2
#define ESP_CACHE_MSYNC_FLAG_INVALIDATE 4
#define DMA2D_DESCRIPTOR_BUFFER_OWNER_DMA 1
#define DMA2D_DESCRIPTOR_PBYTE_3B0_PER_PIXEL 3
void *memcpy(void *d, const void *s, size_t n) {
    for (size_t i=0;i<n;i++) ((uint8_t*)d)[i]=((const uint8_t*)s)[i]; return d;
}
void *memset(void *d, int b, size_t n) {
    for (size_t i=0;i<n;i++) ((uint8_t*)d)[i]=(uint8_t)b; return d;
}
typedef struct {
    unsigned vb_size,hb_length,va_size,ha_length,dma2d_en,suc_eof,owner,pbyte,err_eof;
    void *buffer;
} dma2d_descriptor_t;
typedef struct {
    dma2d_descriptor_t *tx_desc,*rx_desc;
    uint8_t *coords,*edges;
    void *pool,*transaction,*done;
    int request;
    esp_err_t result;
    bool busy,failed;
    const char *operation,*probe_phase;
    atomic_uint dma_stage;
    unsigned side;
    bool (*work)(void *);
    void *work_arg;
    unsigned long long driver_us, wait_us;
} edge_compute_t;
static edge_compute_t *current;
static dma2d_descriptor_t tx_memory,rx_memory;
static unsigned cache_calls,enqueue_calls,cache_fail,enqueue_fail,timeout,rx_error;
static unsigned signalled,tx_visible,rx_visible,inputs_visible,outputs_clean,protocol_error;
static bool edge_dma_done(dma2d_channel_handle_t, dma2d_event_data_t *, void *);
static bool edge_signal(edge_compute_t *s) { (void)s; signalled=1; return false; }
static unsigned work_units,work_delay;
static long long clock_us;
static long long esp_timer_get_time(void) { clock_us+=1000; return clock_us; }
static bool test_work(void *arg) { (void)arg; ++work_units; return true; }
static int xSemaphoreTake(void *done, unsigned ticks) {
    (void)done; (void)ticks; return !timeout && signalled && (!current->work || work_units>=work_delay || ticks!=0);
}
'''

MOCKS = r'''
static esp_err_t esp_cache_msync(void *p, size_t n, int flags) {
    if (++cache_calls==cache_fail) return CACHE_ERROR;
    if (p==current->tx_desc || p==current->rx_desc) {
        if (n!=EDGE_DESC_BYTES || (size_t)p % EDGE_ALIGN) protocol_error=1;
        if (flags & ESP_CACHE_MSYNC_FLAG_DIR_C2M) {
            if (enqueue_calls) protocol_error=2;
            if (p==current->tx_desc) { tx_memory=*current->tx_desc; tx_visible=1; }
            else { rx_memory=*current->rx_desc; rx_visible=1; }
        } else {
            if (!signalled || p!=current->rx_desc) protocol_error=3;
            *current->rx_desc=rx_memory;
        }
    } else if (p==current->coords) {
        if (n!=current->side*current->side*3U || flags!=ESP_CACHE_MSYNC_FLAG_DIR_C2M) protocol_error=4;
        inputs_visible=1;
    } else if (p==current->edges) {
        if (n!=current->side*current->side*3U) protocol_error=5;
        if (flags & ESP_CACHE_MSYNC_FLAG_DIR_C2M) outputs_clean=1;
        else if (!signalled || current->rx_desc->owner) protocol_error=6;
    } else protocol_error=7;
    return ESP_OK;
}
static esp_err_t dma2d_enqueue(void *pool, void *request, void *transaction) {
    (void)pool; (void)request; (void)transaction; ++enqueue_calls;
    if (enqueue_fail) return ENQUEUE_ERROR;
    if (!tx_visible || !rx_visible || !inputs_visible || !outputs_clean ||
        tx_memory.owner!=1 || tx_memory.suc_eof!=1 || tx_memory.buffer!=current->coords ||
        tx_memory.hb_length!=current->side || tx_memory.vb_size!=current->side || tx_memory.pbyte!=3 ||
        rx_memory.owner!=1 || rx_memory.suc_eof!=0 || rx_memory.buffer!=current->edges)
        protocol_error=8;
    if (!timeout) {
        rx_memory.err_eof=rx_error;
        rx_memory.owner=0;
        /* CPU's stale copy deliberately disagrees with the DMA result. */
        current->rx_desc->err_eof=!rx_error;
        edge_dma_done(0,0,current);
    }
    return ESP_OK;
}
'''

CHECKS = r'''
static _Alignas(EDGE_ALIGN) uint8_t tx_storage[EDGE_DESC_BYTES],rx_storage[EDGE_DESC_BYTES];
static uint8_t coords[EDGE_MAX_BYTES],edges[EDGE_MAX_BYTES];
static edge_compute_t state;
static void reset(void) {
    memset(&state,0,sizeof(state));
    memset(tx_storage,0,sizeof(tx_storage)); memset(rx_storage,0,sizeof(rx_storage));
    state.tx_desc=(void*)tx_storage; state.rx_desc=(void*)rx_storage;
    state.coords=coords; state.edges=edges; state.probe_phase="test";
    state.side=16;
    current=&state; work_units=work_delay=0; clock_us=0;
    cache_calls=enqueue_calls=cache_fail=enqueue_fail=timeout=rx_error=0;
    signalled=tx_visible=rx_visible=inputs_visible=outputs_clean=protocol_error=0;
}
__declspec(dllexport) int run_checks(void) {
    reset();
    if(edge_csc(&state)!=ESP_OK || protocol_error || cache_calls!=6 || state.busy) return 1;
    for(unsigned side=32;side<=64;side*=2) {
        reset(); state.side=side;
        if(edge_csc(&state)!=ESP_OK || protocol_error || cache_calls!=6 || state.busy) return 40+side;
    }
    reset(); rx_error=1;
    if(edge_csc(&state)!=ESP_FAIL || protocol_error || cache_calls!=5 || state.busy) return 2;
    for(unsigned i=1;i<=6;++i) {
        reset(); cache_fail=i;
        if(edge_csc(&state)!=CACHE_ERROR || protocol_error || cache_calls!=i || state.busy) return 10+i;
        if(enqueue_calls!=(i>4)) return 20+i;
    }
    reset(); enqueue_fail=1;
    if(edge_csc(&state)!=ENQUEUE_ERROR || state.busy || cache_calls!=4) return 30;
    reset(); timeout=1;
    if(edge_csc(&state)!=ESP_ERR_TIMEOUT || !state.busy || !state.failed || cache_calls!=4) return 31;
    if(edge_csc(&state)!=ESP_ERR_INVALID_STATE || enqueue_calls!=1 || cache_calls!=4) return 32;
    reset(); state.work=test_work; work_delay=3;
    if(edge_csc(&state)!=ESP_OK || work_units!=3 || state.busy || protocol_error) return 35;
    reset(); state.work=test_work;
    if(edge_csc(&state)!=ESP_OK || work_units || state.busy) return 36;
    reset(); timeout=1; state.work=test_work;
    if(edge_csc(&state)!=ESP_ERR_TIMEOUT || !state.busy || !state.failed || !work_units || work_units>1000) return 37;
    reset(); state.busy=true;
    if(edge_csc(&state)!=ESP_ERR_INVALID_STATE || enqueue_calls || cache_calls) return 33;
    reset(); state.failed=true;
    if(edge_csc(&state)!=ESP_ERR_INVALID_STATE || enqueue_calls || cache_calls) return 34;
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--clang', required=True, type=Path)
    p.add_argument('--lld', required=True, type=Path)
    p.add_argument('--build-dir', type=Path, default=Path('build-ppa-dma-host'))
    args = p.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = (root/'components/grape/src/grape_gpu_ppa.c').read_text()
    # Ensure real allocations isolate the cache lines that the tested code syncs.
    assert function(source, 'edge_alloc').count(
        'heap_caps_aligned_calloc(EDGE_ALIGN, 1, EDGE_DESC_BYTES, EDGE_CAPS)') == 2
    defines = '\n'.join(line for line in source.splitlines()
                        if line.startswith(('#define EDGE_SIDE ', '#define EDGE_PIXELS ',
                                            '#define EDGE_BYTES ', '#define EDGE_ALIGN ',
                                            '#define EDGE_DESC_BYTES ', '#define EDGE_MAX_SIDE ', '#define EDGE_MAX_BYTES ')))
    code = PRELUDE + defines + '\n' + MOCKS
    code += ''.join(function(source, name) for name in ('edge_dma_done', 'edge_wait', 'edge_csc'))
    code += CHECKS
    out = args.build_dir.resolve(); out.mkdir(parents=True, exist_ok=True)
    (out/'dma.c').write_text(code)
    subprocess.run([str(args.clang), '--target=x86_64-pc-windows-msvc', '-std=c11', '-O2',
                    '-ffreestanding', '-fno-builtin', '-c', str(out/'dma.c'),
                    '-o', str(out/'dma.obj')], check=True)
    subprocess.run([str(args.lld), '-flavor', 'link', '/dll', '/noentry', '/machine:x64',
                    '/out:'+str(out/'dma.dll'), str(out/'dma.obj')], check=True)
    result = ct.CDLL(str(out/'dma.dll')).run_checks()
    assert result == 0, result
    print('DMA handoff: publication, stale status, 6 cache errors, enqueue failure, '
          'timeout quarantine, busy/failed rejection passed')


if __name__ == '__main__':
    main()
