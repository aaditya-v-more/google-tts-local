#include "guest.h"
#include "guest_native.h"

/* Runs in either ABI mode. No part of this file may call a Darwin library. */
static void lock_arena(Guest *g) {
    while (__atomic_exchange_n(&g->native_alloc_lock, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&g->native_alloc_lock, __ATOMIC_RELAXED)) __asm__ volatile("yield");
    }
}
static void unlock_arena(Guest *g) { __atomic_store_n(&g->native_alloc_lock, 0, __ATOMIC_RELEASE); }

static void *alloc_raw_locked(Guest *g, size_t size, size_t alignment) {
    if (!alignment || (alignment & (alignment - 1)) || g->arena_used > SIZE_MAX - alignment + 1) return NULL;
    size_t offset = (g->arena_used + alignment - 1) & ~(alignment - 1);
    if (offset > g->arena_size || size > g->arena_size - offset) return NULL;
    g->arena_used = offset + size;
    return g->arena + offset;
}

void *guest_native_alloc_raw(Guest *g, size_t size, size_t alignment) {
    Guest *shared = g->master;
    lock_arena(shared);
    void *p = alloc_raw_locked(shared, size, alignment);
    unlock_arena(shared);
    return p;
}

static size_t allocation_class(size_t size, size_t *capacity) {
    if (size <= 4096) {
        size_t rounded = (size + 15) & ~(size_t)15;
        *capacity = rounded;
        return rounded / 16 - 1;
    }
    unsigned power = 8 * sizeof(size_t) - 1 - (unsigned)__builtin_clzl(size);
    size_t base = (size_t)1 << power, step = base >> 3;
    size_t offset = (size - base + step - 1) / step;
    *capacity = base + offset * step;
    return 256 + (power - 12) * 8 + offset;
}

void *guest_native_heap_alloc(Guest *g, size_t alignment, size_t size) {
    if (alignment < 16) alignment = 16;
    if ((alignment & (alignment - 1)) || size > g->master->arena_size) return NULL;
    if (size < 8) size = 8;
    if (alignment > SIZE_MAX - 16 || size > SIZE_MAX - 16 - alignment) return NULL;
    size_t capacity = 0, bin = allocation_class(size, &capacity);
    if (bin >= 512 || capacity > SIZE_MAX - 16 - alignment) return NULL;
    Guest *shared = g->master;
    lock_arena(shared);
    size_t end = bin + 8 < 512 ? bin + 8 : 512;
    for (size_t i = bin; i < end; ++i) {
        uint64_t *previous = &shared->free_bins[i];
        while (*previous) {
            uint64_t *header = (uint64_t *)(uintptr_t)*previous;
            uint64_t *next = header + 2;
            if (header[0] >= size && !((uintptr_t)next & (alignment - 1))) {
                *previous = *next;
                header[1] = UINT64_C(0x4754545348454150);
                unlock_arena(shared);
                return next;
            }
            previous = next;
        }
    }
    uint8_t *raw = alloc_raw_locked(shared, capacity + 16 + alignment - 1, 16);
    if (!raw) { unlock_arena(shared); return NULL; }
    uintptr_t address = ((uintptr_t)raw + 16 + alignment - 1) & ~(alignment - 1);
    uint64_t *header = (uint64_t *)address - 2;
    header[0] = capacity;
    header[1] = UINT64_C(0x4754545348454150);
    unlock_arena(shared);
    return (void *)address;
}

void guest_native_heap_free(Guest *g, void *pointer) {
    if (!pointer) return;
    Guest *shared = g->master;
    uintptr_t p = (uintptr_t)pointer, base = (uintptr_t)shared->arena;
    if (p < base + 16 || p - base >= shared->arena_size || (p & 15)) __builtin_trap();
    lock_arena(shared);
    uint64_t *header = (uint64_t *)pointer - 2;
    if (header[1] != UINT64_C(0x4754545348454150)) __builtin_trap();
    header[1] = 0;
    size_t capacity = 0, bin = allocation_class((size_t)header[0], &capacity);
    if (bin >= 512) __builtin_trap();
    *(uint64_t *)pointer = shared->free_bins[bin];
    shared->free_bins[bin] = (uintptr_t)header;
    unlock_arena(shared);
}

static Guest *current_guest(void) {
    uintptr_t tsd;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tsd));
    return (Guest *)((uintptr_t *)tsd)[guest_native_current_key];
}

void *guest_native_fast_malloc(size_t size) {
    return guest_native_heap_alloc(current_guest(), 16, size);
}
void guest_native_fast_free(void *pointer) {
    guest_native_heap_free(current_guest(), pointer);
}
