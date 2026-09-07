#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "esp_idf_version.h"
#include "esp_log.h"
#include "grape_internal.h"
#include "grape_gpu_ppa.h"
#include "grape_ppa_edge_math.h"

/* The custom CSC register bridge is deliberately pinned to the inspected IDF.
 * Other versions retain the feature registry entry and the original CPU path. */
#if CONFIG_GRAPE_PPA_TRIANGLE_COMPUTE && CONFIG_IDF_TARGET_ESP32P4 && \
    ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(5, 5, 4) && \
    !CONFIG_DMA2D_ISR_IRAM_SAFE
#define GRAPE_PPA_COMPUTE_AVAILABLE 1
#else
#define GRAPE_PPA_COMPUTE_AVAILABLE 0
#endif

void grape_gpu_ppa_feature_init(grape_context_t *grape)
{
    grape_feature_set_availability(grape, GRAPE_FEATURE_PPA_TRIANGLE_OVERLAP,
        GRAPE_PPA_COMPUTE_AVAILABLE,
        GRAPE_PPA_COMPUTE_AVAILABLE ? GRAPE_FEATURE_UNAVAILABLE_NONE
                                    : GRAPE_FEATURE_UNAVAILABLE_NOT_COMPILED);
    grape_feature_set_availability(grape, GRAPE_FEATURE_PPA_TRIANGLE_BATCH,
        GRAPE_PPA_COMPUTE_AVAILABLE,
        GRAPE_PPA_COMPUTE_AVAILABLE ? GRAPE_FEATURE_UNAVAILABLE_NONE
                                    : GRAPE_FEATURE_UNAVAILABLE_NOT_COMPILED);
#if GRAPE_PPA_COMPUTE_AVAILABLE && CONFIG_GRAPE_PPA_TRIANGLE_VALIDATE
    grape_feature_set_availability(grape, GRAPE_FEATURE_PPA_TRIANGLE_VALIDATE,
        true, GRAPE_FEATURE_UNAVAILABLE_NONE);
#else
    grape_feature_set_availability(grape, GRAPE_FEATURE_PPA_TRIANGLE_VALIDATE,
        false, GRAPE_FEATURE_UNAVAILABLE_NOT_COMPILED);
#endif
    grape_feature_set_availability(grape, GRAPE_FEATURE_PPA_TRIANGLE_EDGES,
        GRAPE_PPA_COMPUTE_AVAILABLE,
        GRAPE_PPA_COMPUTE_AVAILABLE ? GRAPE_FEATURE_UNAVAILABLE_NONE
                                    : GRAPE_FEATURE_UNAVAILABLE_NOT_COMPILED);
}

esp_err_t grape_gpu_get_ppa_triangle_stats(const grape_gpu_context_t *context,
                                           grape_gpu_ppa_triangle_stats_t *out)
{
    if (!context || !out) return ESP_ERR_INVALID_ARG;
    *out = context->ppa_triangle_stats;
    return ESP_OK;
}

#if GRAPE_PPA_COMPUTE_AVAILABLE
#include <stdatomic.h>
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_private/dma2d.h"
/* Only this translation unit uses the version-pinned private channel layout.
 * No IDF source modification or guessed channel number is required. */
#include "dma2d_priv.h"
#include "soc/dma2d_channel.h"

#define EDGE_SIDE 16U
#define EDGE_PIXELS (EDGE_SIDE * EDGE_SIDE)
#define EDGE_BYTES (EDGE_PIXELS * 3U)
#define EDGE_MAX_SIDE 64U
#define EDGE_MAX_BYTES (EDGE_MAX_SIDE * EDGE_MAX_SIDE * 3U)
#define EDGE_ALIGN 128U
#define EDGE_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)
/* Isolate descriptors from unrelated CPU data, including allocator metadata.
 * P4 internal SRAM is cached too; DMA writes RX status back into this storage. */
#define EDGE_DESC_BYTES ((sizeof(dma2d_descriptor_t) + EDGE_ALIGN - 1) & ~(EDGE_ALIGN - 1))

static const char *TAG = "grape_ppa_compute";

typedef struct {
    dma2d_pool_handle_t pool;
    ppa_client_handle_t blend;
    SemaphoreHandle_t done;
    dma2d_trans_t *transaction;
    dma2d_trans_config_t request;
    dma2d_descriptor_t *tx_desc;
    dma2d_descriptor_t *rx_desc;
    uint8_t *coords;
    uint8_t *edges;
    uint8_t *black;
    uint8_t *mask;
    grape_ppa_edge_coeff_t coeff[3];
    esp_err_t result;
    const char *probe_phase;
    const char *operation;
    atomic_uint dma_stage; /* 0 queued, 1 picked, 2 starting, 3 EOF callback */
    /* Owner-task only; ISR callbacks never dereference the work context. */
    bool (*work)(void *);
    void *work_arg;
    uint64_t driver_us, wait_us;
    unsigned side;
    int round_bias;
    bool busy;
    bool ready;
    bool failed;
} edge_compute_t;

static bool edge_signal(edge_compute_t *s)
{
    BaseType_t wake = pdFALSE;
    if (xPortInIsrContext()) xSemaphoreGiveFromISR(s->done, &wake);
    else xSemaphoreGive(s->done);
    return wake == pdTRUE;
}

static bool edge_dma_done(dma2d_channel_handle_t channel, dma2d_event_data_t *event,
                           void *arg)
{
    (void)channel;
    (void)event;
    edge_compute_t *s = arg;
    /* Descriptor writeback is not coherent with the CPU cache. Read status in
     * the waiting task after M2C synchronization, not from this callback. */
    atomic_store(&s->dma_stage, 3);
    return edge_signal(s);
}

static bool edge_ppa_done(ppa_client_handle_t client, ppa_event_data_t *event, void *arg)
{
    (void)client;
    (void)event;
    return edge_signal(arg);
}

static bool edge_dma_picked(uint32_t count, const dma2d_trans_channel_info_t *channels,
                             void *arg)
{
    edge_compute_t *s = arg;
    atomic_store(&s->dma_stage, 1);
    dma2d_channel_handle_t tx = NULL, rx = NULL;
    for (uint32_t i = 0; i < count; ++i) {
        if (channels[i].dir == DMA2D_CHANNEL_DIRECTION_TX) tx = channels[i].chan;
        else rx = channels[i].chan;
    }
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (!tx || !rx) goto fail;
    dma2d_trigger_t trigger = {DMA2D_TRIG_PERIPH_M2M, SOC_DMA2D_TRIG_PERIPH_M2M_TX};
    if ((ret = dma2d_connect(tx, &trigger)) != ESP_OK) goto fail;
    trigger.periph_sel_id = SOC_DMA2D_TRIG_PERIPH_M2M_RX;
    if ((ret = dma2d_connect(rx, &trigger)) != ESP_OK) goto fail;
    const dma2d_csc_config_t csc = {.tx_csc_option = DMA2D_CSC_TX_RGB888_TO_YUV444_601};
    if ((ret = dma2d_configure_color_space_conversion(tx, &csc)) != ESP_OK) goto fail;

    /* Connect/configure reset CSC. Install our rows afterwards, on the channel
     * allocated for THIS transaction, and before either channel starts. */
    volatile dma2d_color_param_group_chn_reg_t *regs =
        &tx->group->hal.dev->out_channel[tx->channel_id].out_color_param_group;
    volatile dma2d_color_param_reg_t *lanes[3] = {&regs->param_h, &regs->param_m, &regs->param_l};
    for (unsigned i = 0; i < 3; ++i) {
        dma2d_color_param_reg_t value = {0};
        value.a = s->coeff[i].a;
        value.b = s->coeff[i].b;
        value.c = s->coeff[i].c;
        value.d = s->coeff[i].d;
        lanes[i]->val[0] = value.val[0];
        lanes[i]->val[1] = value.val[1];
    }
    dma2d_rx_event_callbacks_t callbacks = {.on_recv_eof = edge_dma_done};
    if ((ret = dma2d_register_rx_event_callbacks(rx, &callbacks, s)) != ESP_OK) goto fail;
    if ((ret = dma2d_set_desc_addr(tx, (intptr_t)s->tx_desc)) != ESP_OK) goto fail;
    if ((ret = dma2d_set_desc_addr(rx, (intptr_t)s->rx_desc)) != ESP_OK) goto fail;
    /* These APIs cannot fail with the acquired non-null channel handles. */
    atomic_store(&s->dma_stage, 2);
    dma2d_start(tx);
    dma2d_start(rx);
    return false;
fail:
    s->result = ret;
    bool yield = false;
    /* No channel has been started: release this transaction's allocation. */
    if (dma2d_force_end(s->transaction, &yield) == ESP_OK)
        yield |= edge_signal(s);
    /* Otherwise leave it quarantined; the caller's bounded wait will expire. */
    return yield;
}

static esp_err_t edge_wait(edge_compute_t *s)
{
    const int64_t deadline = esp_timer_get_time() + 1000000;
    for (;;) {
        if (s->work && xSemaphoreTake(s->done, 0) == pdTRUE) break;
        const int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) goto timeout;
        /* Work claims disjoint CPU blocks. Completion is checked between four-
         * row chunks; the same task advances CSC -> PPA -> commit in order. */
        if (s->work && s->work(s->work_arg)) continue;
        TickType_t ticks = pdMS_TO_TICKS((remaining + 999) / 1000);
        if (!ticks) ticks = 1;
        const int64_t started = esp_timer_get_time();
        const BaseType_t done = xSemaphoreTake(s->done, ticks);
        s->wait_us += esp_timer_get_time() - started;
        if (done == pdTRUE) break;
        goto timeout;
    }
    s->busy = false;
    return s->result;
timeout:
    s->failed = true;
    ESP_LOGE(TAG, "Timeout: probe=%s operation=%s DMA stage=%u "
             "(0=queued 1=picked 2=starting 3=EOF)",
             s->probe_phase, s->operation, atomic_load(&s->dma_stage));
    return ESP_ERR_TIMEOUT;
}

static esp_err_t edge_csc(edge_compute_t *s)
{
    int64_t timing = esp_timer_get_time();
    const size_t bytes = s->side * s->side * 3U;
    if (s->busy || s->failed) return ESP_ERR_INVALID_STATE;
    *s->tx_desc = (dma2d_descriptor_t){
        .vb_size = s->side, .hb_length = s->side, .va_size = s->side, .ha_length = s->side,
        .dma2d_en = 1, .suc_eof = 1, .owner = DMA2D_DESCRIPTOR_BUFFER_OWNER_DMA,
        .pbyte = DMA2D_DESCRIPTOR_PBYTE_3B0_PER_PIXEL, .buffer = s->coords,
    };
    *s->rx_desc = *s->tx_desc;
    s->rx_desc->suc_eof = 0; /* Written by DMA, as in the IDF M2M tests. */
    s->rx_desc->buffer = s->edges;
    esp_err_t ret = esp_cache_msync(s->coords, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    if (ret != ESP_OK) return ret;
    ret = esp_cache_msync(s->edges, bytes,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    if (ret != ESP_OK) return ret;
    ret = esp_cache_msync(s->tx_desc, EDGE_DESC_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    if (ret != ESP_OK) return ret;
    ret = esp_cache_msync(s->rx_desc, EDGE_DESC_BYTES,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    if (ret != ESP_OK) return ret;
    s->result = ESP_OK;
    s->operation = "DMA2D CSC";
    atomic_store(&s->dma_stage, 0);
    s->busy = true;
    ret = dma2d_enqueue(s->pool, &s->request, s->transaction);
    if (ret != ESP_OK) { s->busy = false; return ret; }
    s->driver_us += esp_timer_get_time() - timing;
    ret = edge_wait(s);
    timing = esp_timer_get_time();
    if (ret != ESP_OK) return ret;
    ret = esp_cache_msync(s->rx_desc, EDGE_DESC_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    if (ret != ESP_OK) return ret;
    if (s->rx_desc->err_eof) return ESP_FAIL;
    ret = esp_cache_msync(s->edges, bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    s->driver_us += esp_timer_get_time() - timing;
    return ret;
}

static esp_err_t edge_key(edge_compute_t *s)
{
    int64_t timing = esp_timer_get_time();
    const size_t bytes = s->side * s->side * 3U;
    if (s->busy || s->failed) return ESP_ERR_INVALID_STATE;
    ppa_blend_oper_config_t config = {
        .in_bg = {.buffer = s->black, .pic_w = s->side, .pic_h = s->side,
                  .block_w = s->side, .block_h = s->side, .blend_cm = PPA_BLEND_COLOR_MODE_RGB888},
        .in_fg = {.buffer = s->edges, .pic_w = s->side, .pic_h = s->side,
                  .block_w = s->side, .block_h = s->side, .blend_cm = PPA_BLEND_COLOR_MODE_RGB888},
        .out = {.buffer = s->mask, .buffer_size = bytes, .pic_w = s->side, .pic_h = s->side,
                .blend_cm = PPA_BLEND_COLOR_MODE_RGB888},
        .bg_ck_en = true, .fg_ck_en = true,
        .bg_ck_rgb_low_thres = {.r = 0, .g = 0, .b = 0},
        .bg_ck_rgb_high_thres = {.r = 255, .g = 255, .b = 255},
        .fg_ck_rgb_low_thres = {.r = 128, .g = 128, .b = 128},
        .fg_ck_rgb_high_thres = {.r = 255, .g = 255, .b = 255},
        .ck_rgb_default_val = {.r = 255, .g = 255, .b = 255},
        .ck_reverse_bg2fg = false,
        .mode = PPA_TRANS_MODE_NON_BLOCKING, .user_data = s,
    };
    s->result = ESP_OK;
    s->operation = "PPA key";
    s->busy = true;
    esp_err_t ret = ppa_do_blend(s->blend, &config);
    if (ret != ESP_OK) { s->busy = false; return ret; }
    s->driver_us += esp_timer_get_time() - timing;
    ret = edge_wait(s);
    timing = esp_timer_get_time();
    if (ret != ESP_OK) return ret;
    ret = esp_cache_msync(s->mask, bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    s->driver_us += esp_timer_get_time() - timing;
    return ret;
}

static void edge_free(edge_compute_t *s)
{
    if (!s) return;
    /* A timeout cannot cancel a queued IDF transaction. Do not free DMA-visible
     * storage or callback state while it may still be used. If it completes
     * late we can reclaim it here; otherwise retain this small quarantined job
     * until reboot. Callbacks deliberately contain no GPU/context pointer. */
    if (s->busy && xSemaphoreTake(s->done, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Retaining unfinished accelerator job until reboot");
        return;
    }
    if (s->blend) ppa_unregister_client(s->blend);
    if (s->pool) dma2d_release_pool(s->pool);
    if (s->done) vSemaphoreDelete(s->done);
    heap_caps_free(s->transaction);
    heap_caps_free(s->tx_desc);
    heap_caps_free(s->rx_desc);
    heap_caps_free(s->coords);
    heap_caps_free(s->edges);
    heap_caps_free(s->black);
    heap_caps_free(s->mask);
    heap_caps_free(s);
}

static esp_err_t edge_alloc(edge_compute_t **out)
{
    edge_compute_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s) return ESP_ERR_NO_MEM;
    atomic_init(&s->dma_stage, 0);
    s->probe_phase = "initialization";
    s->side = EDGE_SIDE;
    esp_err_t ret = ESP_ERR_NO_MEM;
    s->done = xSemaphoreCreateBinary();
    s->transaction = heap_caps_calloc(1, SIZEOF_DMA2D_TRANS_T, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s->tx_desc = heap_caps_aligned_calloc(EDGE_ALIGN, 1, EDGE_DESC_BYTES, EDGE_CAPS);
    s->rx_desc = heap_caps_aligned_calloc(EDGE_ALIGN, 1, EDGE_DESC_BYTES, EDGE_CAPS);
    s->coords = heap_caps_aligned_calloc(EDGE_ALIGN, 1, EDGE_MAX_BYTES, EDGE_CAPS);
    s->edges = heap_caps_aligned_calloc(EDGE_ALIGN, 1, EDGE_MAX_BYTES, EDGE_CAPS);
    s->black = heap_caps_aligned_calloc(EDGE_ALIGN, 1, EDGE_MAX_BYTES, EDGE_CAPS);
    s->mask = heap_caps_aligned_calloc(EDGE_ALIGN, 1, EDGE_MAX_BYTES, EDGE_CAPS);
    if (!s->done || !s->transaction || !s->tx_desc || !s->rx_desc ||
        !s->coords || !s->edges || !s->black || !s->mask) goto fail;
    const dma2d_pool_config_t pool_config = {.pool_id = 0};
    if ((ret = dma2d_acquire_pool(&pool_config, &s->pool)) != ESP_OK) goto fail;
    const ppa_client_config_t client = {.oper_type = PPA_OPERATION_BLEND, .max_pending_trans_num = 1};
    if ((ret = ppa_register_client(&client, &s->blend)) != ESP_OK) goto fail;
    const ppa_event_callbacks_t callbacks = {.on_trans_done = edge_ppa_done};
    if ((ret = ppa_client_register_event_callbacks(s->blend, &callbacks)) != ESP_OK) goto fail;
    s->request = (dma2d_trans_config_t){
        .tx_channel_num = 1, .rx_channel_num = 1,
        .channel_flags = DMA2D_CHANNEL_FUNCTION_FLAG_SIBLING | DMA2D_CHANNEL_FUNCTION_FLAG_TX_CSC,
        .on_job_picked = edge_dma_picked, .user_config = s,
    };
    *out = s;
    return ESP_OK;
fail:
    edge_free(s);
    return ret;
}

static void edge_coordinates(edge_compute_t *s)
{
    for (unsigned y = 0; y < s->side; ++y) {
        for (unsigned x = 0; x < s->side; ++x) {
            unsigned p = (y * s->side + x) * 3;
            s->coords[p] = 0; s->coords[p + 1] = y; s->coords[p + 2] = x;
        }
    }
}

static esp_err_t edge_probe(edge_compute_t *s)
{
    /* Basis/lane/full-byte-range probe; inputs are logical (high,mid,low). */
    s->probe_phase = "CSC identity";
    ESP_LOGI(TAG, "Probe: %s", s->probe_phase);
    for (unsigned i = 0; i < EDGE_PIXELS; ++i) {
        s->coords[i * 3] = (i * 151) & 255;
        s->coords[i * 3 + 1] = (i * 73) & 255;
        s->coords[i * 3 + 2] = i;
    }
    s->coeff[0] = (grape_ppa_edge_coeff_t){256, 0, 0, 0};
    s->coeff[1] = (grape_ppa_edge_coeff_t){0, 256, 0, 0};
    s->coeff[2] = (grape_ppa_edge_coeff_t){0, 0, 256, 0};
    esp_err_t ret = edge_csc(s);
    if (ret != ESP_OK) return ret;
    if (memcmp(s->coords, s->edges, EDGE_BYTES)) return ESP_ERR_INVALID_RESPONSE;

    /* Distinguish floor from round-to-nearest and test signed coefficients. */
    s->probe_phase = "CSC rounding";
    ESP_LOGI(TAG, "Probe: %s", s->probe_phase);
    s->coeff[0] = (grape_ppa_edge_coeff_t){1, 0, 0, 32640};
    s->coeff[1] = (grape_ppa_edge_coeff_t){0, 1, 0, 32641};
    s->coeff[2] = (grape_ppa_edge_coeff_t){-3, 2, 1, 33024};
    ret = edge_csc(s);
    if (ret != ESP_OK) return ret;
    unsigned matches = 0;
    for (int bias = 0; bias <= 128; bias += 128) {
        bool match = true;
        for (unsigned i = 0; i < EDGE_PIXELS; ++i) {
            for (unsigned lane = 0; lane < 3; ++lane) {
                const grape_ppa_edge_coeff_t *c = &s->coeff[lane];
                int n = c->a * s->coords[i * 3 + 2] + c->b * s->coords[i * 3 + 1] +
                        c->c * s->coords[i * 3] + c->d + bias;
                match &= s->edges[i * 3 + 2 - lane] == n / 256;
            }
        }
        if (match) { ++matches; s->round_bias = bias; }
    }
    if (matches != 1) return ESP_ERR_INVALID_RESPONSE;
    /* Exercise all eight Boolean combinations at the key threshold. */
    s->probe_phase = "PPA thresholds";
    ESP_LOGI(TAG, "Probe: %s", s->probe_phase);
    for (unsigned i = 0; i < EDGE_PIXELS; ++i)
        for (unsigned lane = 0; lane < 3; ++lane)
            s->edges[i * 3 + lane] = (i & (1U << lane)) ? 128 : 127;
    ret = edge_key(s);
    if (ret != ESP_OK) return ret;
    for (unsigned i = 0; i < EDGE_PIXELS; ++i)
        for (unsigned lane = 0; lane < 3; ++lane)
            if (s->mask[i * 3 + lane] != ((i & 7) == 7 ? 255 : 0))
                return ESP_ERR_INVALID_RESPONSE;

    edge_coordinates(s);
    s->probe_phase = "CSC/PPA triangle";
    ESP_LOGI(TAG, "Probe: %s", s->probe_phase);
    /* Real CSC -> PPA triangle, including the excluded diagonal edge. */
    const int64_t row[3] = {-2, -2, 12};
    const int64_t dx[3] = {1, 0, -1}, dy[3] = {0, 1, -1};
    for (unsigned i = 0; i < 3; ++i)
        if (!grape_ppa_edge_encode(row[i], dx[i], dy[i], 16, 16,
                                  s->round_bias, &s->coeff[i])) return ESP_FAIL;
    ret = edge_csc(s);
    if (ret == ESP_OK) ret = edge_key(s);
    if (ret != ESP_OK) return ret;
    for (unsigned y = 0; y < EDGE_SIDE; ++y)
        for (unsigned x = 0; x < EDGE_SIDE; ++x)
            for (unsigned lane = 0; lane < 3; ++lane)
                if (s->mask[(y * EDGE_SIDE + x) * 3 + lane] !=
                    (x >= 2 && y >= 2 && x + y <= 12 ? 255 : 0))
                    return ESP_ERR_INVALID_RESPONSE;
    s->ready = true;
    s->probe_phase = "render";
    return ESP_OK;
}

static void edge_fail(grape_gpu_context_t *context, esp_err_t ret)
{
    edge_compute_t *s = context->ppa_triangle_state;
    if (s) s->failed = true;
    context->ppa_triangle_stats.last_error = ret;
    context->ppa_triangle_stats.ready = false;
    ++context->ppa_triangle_stats.errors;
    if (ret == ESP_ERR_TIMEOUT) ++context->ppa_triangle_stats.timeouts;
    grape_feature_set_availability(context->grape, GRAPE_FEATURE_PPA_TRIANGLE_EDGES,
        false, GRAPE_FEATURE_UNAVAILABLE_INIT_FAILED);
    ESP_LOGE(TAG, "Triangle compute disabled after %s; using CPU", esp_err_to_name(ret));
}

static esp_err_t edge_prepare(grape_gpu_context_t *context)
{
    edge_compute_t *s = context->ppa_triangle_state;
    esp_err_t ret = ESP_OK;
    if (!s) {
        ret = edge_alloc(&s);
        if (ret != ESP_OK) { edge_fail(context, ret); return ret; }
        context->ppa_triangle_state = s;
    }
    if (s->failed) return ESP_ERR_INVALID_STATE;
    if (s->ready) return ESP_OK;
    ret = edge_probe(s);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Probe failed: %s (%s)", s->probe_phase, esp_err_to_name(ret));
        edge_fail(context, ret);
        return ret;
    }
    context->ppa_triangle_stats.ready = true;
    context->ppa_triangle_stats.round_bias = s->round_bias;
    ESP_LOGI(TAG, "CSC lane/rounding and PPA triangle probes passed (bias=%d)", s->round_bias);
    return ESP_OK;
}

esp_err_t grape_gpu_ppa_triangle_self_test(grape_gpu_context_t *context)
{
    if (!context) return ESP_ERR_INVALID_ARG;
    if (context->render_pass_active) return ESP_ERR_INVALID_STATE;
    if (!grape_feature_is_active(context->grape, GRAPE_FEATURE_PPA_TRIANGLE_EDGES))
        return ESP_ERR_NOT_SUPPORTED;
    return edge_prepare(context);
}

static bool edge_tile_coeff(const grape_gpu_triangle_setup_t *setup, int64_t x, int64_t y,
                             unsigned side, int bias, grape_ppa_edge_coeff_t out[3], int64_t rows[3])
{
    const int64_t starts[3] = {setup->row_e0, setup->row_e1, setup->row_e2};
    const int64_t sx[3] = {setup->e0_step_x, setup->e1_step_x, setup->e2_step_x};
    const int64_t sy[3] = {setup->e0_step_y, setup->e1_step_y, setup->e2_step_y};
    for (unsigned i = 0; i < 3; ++i) {
        int64_t dx, dy, row;
        if (__builtin_mul_overflow(x - setup->min_x, sx[i], &dx) ||
            __builtin_mul_overflow(y - setup->min_y, sy[i], &dy) ||
            __builtin_add_overflow(starts[i], dx, &row) ||
            __builtin_add_overflow(row, dy, &row) ||
            !grape_ppa_edge_encode(row, sx[i], sy[i], side, side, bias, &out[i])) return false;
        rows[i] = row;
    }
    return true;
}

typedef struct {
    grape_gpu_context_t *context;
    const grape_gpu_triangle_setup_t *setup;
    grape_color_t color;
    unsigned side;
    int bias;
    int64_t next_x, next_y;
    grape_gpu_triangle_setup_t cpu;
    int32_t cpu_end_y;
    bool cpu_active;
} edge_work_t;

static bool edge_claim(edge_work_t *w, int64_t *x, int64_t *y)
{
    if (w->next_y > w->setup->max_y) return false;
    *x = w->next_x; *y = w->next_y;
    w->next_x += w->side;
    if (w->next_x > w->setup->max_x) {
        w->next_x = w->setup->min_x;
        w->next_y += w->side;
    }
    return true;
}

static bool edge_cpu_step(void *arg)
{
    edge_work_t *w = arg;
    const int64_t started = esp_timer_get_time();
    if (!w->cpu_active) {
        int64_t x, y, rows[3];
        if (!edge_claim(w, &x, &y)) return false;
        grape_ppa_edge_coeff_t unused[3];
        /* The complete triangle was preflighted. Keep the in-flight hardware
         * coefficients untouched; CPU block ownership is separate. */
        bool fits = edge_tile_coeff(w->setup, x, y, w->side, w->bias, unused, rows);
        assert(fits);
        w->cpu = *w->setup;
        w->cpu.min_x = x; w->cpu.min_y = y;
        w->cpu.max_x = x + w->side - 1 < w->setup->max_x ? x + w->side - 1 : w->setup->max_x;
        w->cpu_end_y = y + w->side - 1 < w->setup->max_y ? y + w->side - 1 : w->setup->max_y;
        w->cpu.row_e0 = rows[0]; w->cpu.row_e1 = rows[1]; w->cpu.row_e2 = rows[2];
        w->cpu_active = true;
        ++w->context->ppa_triangle_stats.cpu_tiles_computed;
    }
    w->cpu.max_y = w->cpu.min_y + 3 < w->cpu_end_y ? w->cpu.min_y + 3 : w->cpu_end_y;
    const int32_t height = w->cpu.max_y - w->cpu.min_y + 1;
    grape_gpu_raster_color_block(w->context, &w->cpu, w->color);
    w->context->ppa_triangle_stats.cpu_pixels_computed +=
        (uint64_t)(w->cpu.max_x - w->cpu.min_x + 1) * height;
    w->cpu_active = w->cpu.max_y < w->cpu_end_y;
    if (w->cpu_active) {
        w->cpu.min_y += height;
        w->cpu.row_e0 += w->cpu.e0_step_y * height;
        w->cpu.row_e1 += w->cpu.e1_step_y * height;
        w->cpu.row_e2 += w->cpu.e2_step_y * height;
    }
    w->context->ppa_triangle_stats.cpu_render_us += esp_timer_get_time() - started;
    return true;
}

bool grape_gpu_ppa_try_triangle(grape_gpu_context_t *context,
                                const grape_gpu_triangle_setup_t *setup, grape_color_t color)
{
    if (!grape_feature_is_active(context->grape, GRAPE_FEATURE_PPA_TRIANGLE_EDGES)) return false;
    grape_gpu_ppa_triangle_stats_t *stats = &context->ppa_triangle_stats;
    ++stats->triangles_seen;
    const grape_gpu_pipeline_desc_t *pipeline = &context->bound_pipeline->desc;
    if (context->sample_count != GRAPE_GPU_SAMPLE_COUNT_1 || color.a != 255 ||
        pipeline->depth.test_enable || pipeline->depth.write_enable ||
        (pipeline->fragment_program != GRAPE_GPU_FRAGMENT_PROGRAM_SOLID_COLOR &&
         pipeline->fragment_program != GRAPE_GPU_FRAGMENT_PROGRAM_PUSH_COLOR)) {
        ++stats->fallback_unsupported;
        return false;
    }
    if (edge_prepare(context) != ESP_OK) return false;
    edge_compute_t *s = context->ppa_triangle_state;
    const int64_t prepare_start = esp_timer_get_time();
    int64_t rows[3];
    /* Amortize each CSC/PPA pair over a larger coordinate grid. Retry with
     * smaller blocks when a coefficient or padded output range cannot fit. */
    unsigned side = grape_feature_is_active(context->grape, GRAPE_FEATURE_PPA_TRIANGLE_BATCH)
                    ? EDGE_MAX_SIDE : EDGE_SIDE;
    while (side > EDGE_SIDE && setup->max_x - setup->min_x + 1 <= side / 2 &&
           setup->max_y - setup->min_y + 1 <= side / 2) side /= 2;
    for (;;) {
        bool fits = true;
        for (int64_t y = setup->min_y; fits && y <= setup->max_y; y += side)
            for (int64_t x = setup->min_x; fits && x <= setup->max_x; x += side)
                fits = edge_tile_coeff(setup, x, y, side, s->round_bias, s->coeff, rows);
        if (fits) break;
        if (side == EDGE_SIDE) { ++stats->fallback_coefficients; return false; }
        side /= 2;
    }
    if (s->side != side) { s->side = side; edge_coordinates(s); }
#if CONFIG_GRAPE_PPA_TRIANGLE_VALIDATE
    const bool validate = grape_feature_is_active(context->grape, GRAPE_FEATURE_PPA_TRIANGLE_VALIDATE);
#endif
    stats->prepare_us += esp_timer_get_time() - prepare_start;
    const bool overlap = grape_feature_is_active(context->grape, GRAPE_FEATURE_PPA_TRIANGLE_OVERLAP);
    edge_work_t work = {.context = context, .setup = setup, .color = color, .side = side,
        .bias = s->round_bias, .next_x = setup->min_x, .next_y = setup->min_y};
    const uint64_t cpu_before = stats->cpu_tiles_computed;
    const int64_t started = esp_timer_get_time();
    bool wrote = false;
    int64_t x, y;
    while (edge_claim(&work, &x, &y)) {
        if (!edge_tile_coeff(setup, x, y, side, s->round_bias, s->coeff, rows)) return false;
        const uint64_t driver_before = s->driver_us, wait_before = s->wait_us;
        s->work = overlap ? edge_cpu_step : NULL;
        s->work_arg = &work;
        esp_err_t ret = edge_csc(s);
        if (ret == ESP_OK) ret = edge_key(s);
        /* No stack reference survives this synchronous triangle call,
         * including errors and quarantined transfers. ISR never uses it. */
        s->work = NULL; s->work_arg = NULL;
        stats->driver_us += s->driver_us - driver_before;
        stats->wait_us += s->wait_us - wait_before;
        if (ret != ESP_OK) { edge_fail(context, ret); return false; }
        ++stats->tiles_computed;
        stats->large_tiles_computed += side > EDGE_SIDE;
        stats->pixels_computed += side * side;
        const unsigned width = setup->max_x - x + 1 < side ? setup->max_x - x + 1 : side;
        const unsigned height = setup->max_y - y + 1 < side ? setup->max_y - y + 1 : side;
#if CONFIG_GRAPE_PPA_TRIANGLE_VALIDATE
        if (validate) {
            const int64_t validation_start = esp_timer_get_time();
            const int64_t sx[3] = {setup->e0_step_x, setup->e1_step_x, setup->e2_step_x};
            const int64_t sy[3] = {setup->e0_step_y, setup->e1_step_y, setup->e2_step_y};
            for (unsigned v = 0; v < height; ++v) {
                for (unsigned u = 0; u < width; ++u) {
                    bool inside = true;
                    for (unsigned edge = 0; edge < 3; ++edge)
                        inside &= rows[edge] + sx[edge] * u + sy[edge] * v >= 0;
                    for (unsigned lane = 0; lane < 3; ++lane) {
                        if (s->mask[(v * side + u) * 3 + lane] != (inside ? 255 : 0)) {
                            ++stats->validation_mismatches;
                            edge_fail(context, ESP_ERR_INVALID_RESPONSE);
                            return false;
                        }
                    }
                    ++stats->pixels_validated;
                }
            }
            stats->validation_us += esp_timer_get_time() - validation_start;
        }
#endif
        const int64_t commit_start = esp_timer_get_time();
        /* CPU commit preserves GRAPE's straight RGBA byte order and alpha.
         * Earlier verified tile writes are idempotent if a later tile
         * fails and the original CPU renderer redraws this triangle. */
        for (unsigned v = 0; v < height; ++v) {
            uint8_t *dst = context->color_attachment->pixels +
                (size_t)(y + v) * context->color_attachment->stride + (size_t)x * 4;
            for (unsigned u = 0; u < width; ++u) {
                if (s->mask[(v * side + u) * 3] == 255) {
                    dst[u * 4] = color.r; dst[u * 4 + 1] = color.g;
                    dst[u * 4 + 2] = color.b; dst[u * 4 + 3] = color.a;
                    wrote = true;
                }
            }
        }
        stats->commit_us += esp_timer_get_time() - commit_start;
        /* Finish only the block already claimed by CPU. The next hardware
         * block comes from the remaining shared cursor. */
        while (work.cpu_active) edge_cpu_step(&work);
    }
    if (overlap && stats->cpu_tiles_computed != cpu_before) ++stats->overlap_triangles;
    stats->compute_us += esp_timer_get_time() - started;
    ++stats->triangles_completed;
    if (wrote) grape_gpu_dirty_add(context, (grape_rect_t){setup->min_x, setup->min_y,
        setup->max_x - setup->min_x + 1, setup->max_y - setup->min_y + 1});
    return true;
}

void grape_gpu_ppa_release(grape_gpu_context_t *context)
{
    edge_free(context->ppa_triangle_state);
    context->ppa_triangle_state = NULL;
}

#else
esp_err_t grape_gpu_ppa_triangle_self_test(grape_gpu_context_t *context)
{
    return context ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_ARG;
}
bool grape_gpu_ppa_try_triangle(grape_gpu_context_t *context,
                                const grape_gpu_triangle_setup_t *setup, grape_color_t color)
{
    (void)context; (void)setup; (void)color;
    return false;
}
void grape_gpu_ppa_release(grape_gpu_context_t *context) { (void)context; }
#endif
