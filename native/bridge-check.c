#include "guest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern void bridge_guest(void);
extern void bridge_alloc_guest(void);
extern void bridge_trap_guest(void), bridge_bad_access_guest(void);
typedef struct { Guest *master; uint64_t malloc_fn, free_fn; int failed; } AllocCheck;
static void *alloc_thread(void *opaque) {
    AllocCheck *check = opaque;
    Guest *g = calloc(1, sizeof *g);
    if (!g) { __atomic_store_n(&check->failed, 1, __ATOMIC_RELAXED); return NULL; }
    guest_thread_clone(g, check->master);
    if (guest_thread_cpu_init(g)) __atomic_store_n(&check->failed, 1, __ATOMIC_RELAXED);
    else {
        uint64_t args[8] = {check->malloc_fn, check->free_fn, 10000}, result = 1;
        if (guest_call(g, (uintptr_t)bridge_alloc_guest, args, &result) || result) __atomic_store_n(&check->failed, 1, __ATOMIC_RELAXED);
    }
    guest_heap_free(g, g->tls);
    guest_heap_free(g, (void *)(uintptr_t)g->errno_slot);
    free(g);
    return NULL;
}
int main(int argc, char **argv) {
    Guest *g = calloc(1, sizeof *g);
    if (!g || guest_create(g)) return 1;
    if (!g->native_mode) {
        guest_destroy(g); free(g);
        puts("in-process bridge: skipped (custom ABI unavailable)");
        return 0;
    }
    if (argc == 2) {
        if (!strcmp(argv[1], "--fault-trap")) guest_call(g, (uintptr_t)bridge_trap_guest, NULL, NULL);
        else if (!strcmp(argv[1], "--fault-access")) guest_call(g, (uintptr_t)bridge_bad_access_guest, NULL, NULL);
        else if (!strcmp(argv[1], "--fault-thread")) {
            uint64_t handle = 0, result = 1;
            uint64_t args[8] = {(uintptr_t)&handle, 0, (uintptr_t)bridge_bad_access_guest, 0};
            if (guest_call(g, guest_import(g, "pthread_create"), args, &result) || result) return 1;
            pthread_join((pthread_t)(uintptr_t)handle, NULL);
        } else return 1;
        fprintf(stderr, "guest fault unexpectedly returned\n");
        return 1;
    }
    uint64_t args[8] = { guest_import(g, "getpid"), (uint64_t)getpid() }, result = 99;
    int status = guest_call(g, (uintptr_t)bridge_guest, args, &result);
    if (status || result) { fprintf(stderr, "bridge failure: status=%d result=%llu\n", status, (unsigned long long)result); return 1; }
    AllocCheck check = {g, guest_import(g, "malloc"), guest_import(g, "free"), 0};
    pthread_t threads[6];
    for (unsigned i = 0; i < 6; ++i) if (pthread_create(&threads[i], NULL, alloc_thread, &check)) return 1;
    for (unsigned i = 0; i < 10000; ++i) {
        void *p = guest_heap_alloc(g, (i & 511) + 16);
        if (!p) return 1;
        guest_heap_free(g, p);
    }
    for (unsigned i = 0; i < 6; ++i) pthread_join(threads[i], NULL);
    if (__atomic_load_n(&check.failed, __ATOMIC_RELAXED)) { fprintf(stderr, "concurrent native allocator failed\n"); return 1; }
    guest_destroy(g);
    free(g);
    puts("in-process bridge: PASS");
    return 0;
}
