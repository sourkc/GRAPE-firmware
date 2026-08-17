#include "grape/grape_benchmark_hooks.h"

#include <string.h>

#include "sdkconfig.h"

#if CONFIG_GRAPE_FUNCTION_PROFILING

typedef struct {
    uintptr_t function_address;
    uint64_t calls;
} grape_function_profile_slot_t;

static grape_function_profile_slot_t s_slots[CONFIG_GRAPE_FUNCTION_PROFILE_MAX_FUNCTIONS];
static bool s_active;
static bool s_overflow;

#define GRAPE_PROFILE_NOINSTRUMENT __attribute__((no_instrument_function))

static GRAPE_PROFILE_NOINSTRUMENT size_t profile_hash(uintptr_t address)
{
    address >>= 2U;
    address ^= address >> 11U;
    address *= (uintptr_t)0x9e3779b1U;
    address ^= address >> 13U;
    return (size_t)(address % CONFIG_GRAPE_FUNCTION_PROFILE_MAX_FUNCTIONS);
}

void GRAPE_PROFILE_NOINSTRUMENT __cyg_profile_func_enter(
    void *this_fn,
    void *call_site
)
{
    (void)call_site;

    if (!s_active || !this_fn) {
        return;
    }

    uintptr_t address = (uintptr_t)this_fn;
    size_t index = profile_hash(address);

    for (size_t probe = 0; probe < CONFIG_GRAPE_FUNCTION_PROFILE_MAX_FUNCTIONS; ++probe) {
        grape_function_profile_slot_t *slot = &s_slots[index];
        uintptr_t current = slot->function_address;

        if (current == address) {
            ++slot->calls;
            return;
        }

        if (current == 0U) {
            slot->function_address = address;
            slot->calls = 1U;
            return;
        }

        ++index;
        if (index == CONFIG_GRAPE_FUNCTION_PROFILE_MAX_FUNCTIONS) {
            index = 0U;
        }
    }

    s_overflow = true;
}

void GRAPE_PROFILE_NOINSTRUMENT __cyg_profile_func_exit(
    void *this_fn,
    void *call_site
)
{
    (void)this_fn;
    (void)call_site;
}

bool GRAPE_PROFILE_NOINSTRUMENT grape_benchmark_function_profile_enabled(void)
{
    return true;
}

size_t GRAPE_PROFILE_NOINSTRUMENT grape_benchmark_function_profile_capacity(void)
{
    return CONFIG_GRAPE_FUNCTION_PROFILE_MAX_FUNCTIONS;
}

void GRAPE_PROFILE_NOINSTRUMENT grape_benchmark_function_profile_reset(void)
{
    s_active = false;
    memset(s_slots, 0, sizeof(s_slots));
    s_overflow = false;
}

void GRAPE_PROFILE_NOINSTRUMENT grape_benchmark_function_profile_start(void)
{
    s_active = true;
}

void GRAPE_PROFILE_NOINSTRUMENT grape_benchmark_function_profile_stop(void)
{
    s_active = false;
}

size_t GRAPE_PROFILE_NOINSTRUMENT grape_benchmark_function_profile_snapshot(
    grape_benchmark_function_profile_entry_t *entries,
    size_t capacity,
    bool *out_overflow
)
{
    if (out_overflow) {
        *out_overflow = s_overflow;
    }

    size_t count = 0U;
    for (size_t i = 0; i < CONFIG_GRAPE_FUNCTION_PROFILE_MAX_FUNCTIONS; ++i) {
        uintptr_t address = s_slots[i].function_address;
        if (address == 0U) {
            continue;
        }

        if (entries && count < capacity) {
            entries[count] = (grape_benchmark_function_profile_entry_t) {
                .function_address = address,
                .calls = s_slots[i].calls,
            };
        }
        ++count;
    }

    return count;
}

#else

bool grape_benchmark_function_profile_enabled(void)
{
    return false;
}

size_t grape_benchmark_function_profile_capacity(void)
{
    return 0U;
}

void grape_benchmark_function_profile_reset(void)
{
}

void grape_benchmark_function_profile_start(void)
{
}

void grape_benchmark_function_profile_stop(void)
{
}

size_t grape_benchmark_function_profile_snapshot(
    grape_benchmark_function_profile_entry_t *entries,
    size_t capacity,
    bool *out_overflow
)
{
    (void)entries;
    (void)capacity;
    if (out_overflow) {
        *out_overflow = false;
    }
    return 0U;
}

#endif
