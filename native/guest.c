#include "guest.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define GUEST_ARENA_SIZE (1024ull * 1024 * 1024)
#define GUEST_STACK_SIZE (8ull * 1024 * 1024)
#define CHECK_HV(expr) do { hv_return_t r_ = (expr); if (r_ != HV_SUCCESS) { \
    fprintf(stderr, "%s failed: %#x\n", #expr, (unsigned)r_); return -1; } } while (0)

extern char guest_fast_start[], guest_fast_end[], guest_fast_memcpy[], guest_fast_memmove[];
extern char guest_fast_memset[], guest_fast_strlen[], guest_fast_strnlen[], guest_fast_strcmp[];
extern char guest_fast_memcmp[], guest_fast_memchr[];
extern char **environ;

void guest_update_timezone(Guest *g) {
    Guest *shared=g->master;tzset();
    char **names=(char **)(uintptr_t)shared->tzname_slot;
    if(!names)return;
    for(unsigned i=0;i<2;++i)strlcpy(names[i],tzname[i] ? tzname[i] : "",256);
    *(int64_t *)(uintptr_t)shared->timezone_slot=(int64_t)timezone;
}

double guest_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

void *guest_alloc(Guest *g, size_t size, size_t alignment) {
    if (!alignment || (alignment & (alignment - 1))) return NULL;
    if (g->native_mode) return guest_native_alloc_raw(g, size, alignment);
    Guest *shared = g->master;
    pthread_mutex_lock(&shared->arena_lock);
    size_t offset = (shared->arena_used + alignment - 1) & ~(alignment - 1);
    if (offset > shared->arena_size || size > shared->arena_size - offset) {
        pthread_mutex_unlock(&shared->arena_lock); return NULL;
    }
    void *result = shared->arena + offset;
    shared->arena_used = offset + size;
    pthread_mutex_unlock(&shared->arena_lock);
    return result;
}

int guest_create(Guest *g) {
    memset(g, 0, sizeof(*g));
    g->master = g;
    pthread_mutex_init(&g->arena_lock, NULL);
    pthread_mutex_init(&g->heap_lock, NULL);
    pthread_mutex_init(&g->once_lock, NULL);
    pthread_mutex_init(&g->import_lock, NULL);
    pthread_mutex_init(&g->map_lock, NULL);
    pthread_cond_init(&g->once_cond, NULL);
    g->tid = g->next_tid = 1;
    const char *backend = getenv("GTTS_NATIVE_BACKEND");
    if (backend && !*backend) backend = NULL;
    if (backend && strcmp(backend, "vm") && strcmp(backend, "inprocess") && strcmp(backend, "macho")) {
        fprintf(stderr, "Unknown GTTS_NATIVE_BACKEND: %s\n", backend); return -1;
    }
    g->native_mode = !(backend && !strcmp(backend, "vm")) && guest_native_available(g);
    if (backend && (!strcmp(backend, "inprocess") || !strcmp(backend, "macho")) && !g->native_mode) {
        fprintf(stderr, "In-process backend unavailable: entitlement, OS, or x18 probe failed.\n"); return -1;
    }
    if (g->native_mode) {
        if (backend && !strcmp(backend, "macho")) g->macho_mode = 1;
        else if (!backend) {
            const char *data = getenv("GTTS_DATA"), *dir = getenv("GTTS_MACHO_DIR");
            char path[PATH_MAX];
            if (dir && *dir && snprintf(path, sizeof path, "%s/metadata.plist", dir) < (int)sizeof path)
                g->macho_mode = access(path, R_OK) == 0;
            else if (data && *data && snprintf(path, sizeof path, "%s/macho/metadata.plist", data) < (int)sizeof path)
                g->macho_mode = access(path, R_OK) == 0;
        }
        g->arena_size = GUEST_ARENA_SIZE;
        g->arena = mmap(NULL, g->arena_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (g->arena == MAP_FAILED) { perror("mmap guest arena"); guest_native_control_stop(g); return -1; }
        g->stack_size = GUEST_STACK_SIZE;
        g->tls = guest_alloc(g, 65536, 16384);
        if (!g->tls) { munmap(g->arena, g->arena_size); guest_native_control_stop(g); return -1; }
        g->tls_used = 0x1000;
        g->current_locale = UINT64_MAX;
        g->tls_stub = guest_native_tls_stub();
        g->fast_base = (uintptr_t)guest_fast_start;
        goto common_slots;
    }
    uint32_t max_ipa = 0;
    CHECK_HV(hv_vm_config_get_max_ipa_size(&max_ipa));
    if (max_ipa < 40) {
        fprintf(stderr, "The VM backend needs a 40-bit guest address space (host supports %u).\n", max_ipa);
        return -1;
    }
    hv_vm_config_t config = hv_vm_config_create();
    if (!config) return -1;
    CHECK_HV(hv_vm_config_set_ipa_size(config, 40));
    CHECK_HV(hv_vm_create(config));
    os_release(config);
    g->arena_size = GUEST_ARENA_SIZE;
    g->arena = mmap(NULL, g->arena_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g->arena == MAP_FAILED) { perror("mmap guest arena"); return -1; }
    uintptr_t addr = (uintptr_t)g->arena;
    if (addr >= (UINT64_C(1) << 40) || g->arena_size > (UINT64_C(1) << 40) - addr) {
        fprintf(stderr, "Host arena %p is outside the 40-bit guest address space.\n", g->arena);
        return -1;
    }
    CHECK_HV(hv_vm_map(g->arena, addr, g->arena_size,
                       HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
    CHECK_HV(hv_vcpu_create(&g->vcpu, &g->exit, NULL));
    CHECK_HV(hv_vcpu_set_reg(g->vcpu, HV_REG_CPSR, 0x3c5)); // EL1h
    CHECK_HV(hv_vcpu_set_sys_reg(g->vcpu, HV_SYS_REG_CPACR_EL1, 3u << 20));

    // With stage 1 disabled, EL1 accesses are Device memory and exclusive
    // atomics fail. Identity-map the 40-bit IPA range as normal memory using
    // 4 KiB translation tables and 1 GiB blocks.
    uint64_t *l0 = guest_alloc(g, 4096, 4096);
    uint64_t *l1a = guest_alloc(g, 4096, 4096);
    uint64_t *l1b = guest_alloc(g, 4096, 4096);
    if (!l0 || !l1a || !l1b) return -1;
    l0[0] = (uintptr_t)l1a | 3;
    l0[1] = (uintptr_t)l1b | 3;
    g->page_table = (uintptr_t)l0;
    const uint64_t block_flags = (1u << 10) | (3u << 8) | 1u; // AF, inner-shareable, block
    for (uint64_t i = 0; i < 512; ++i) {
        l1a[i] = (i << 30) | block_flags;
        l1b[i] = ((i + 512) << 30) | block_flags;
    }
    CHECK_HV(hv_vcpu_set_sys_reg(g->vcpu, HV_SYS_REG_MAIR_EL1, 0xff));
    uint64_t tcr = 24 | (1u << 8) | (1u << 10) | (3u << 12) |
                   (1u << 23) | (UINT64_C(2) << 32);
    CHECK_HV(hv_vcpu_set_sys_reg(g->vcpu, HV_SYS_REG_TCR_EL1, tcr));
    CHECK_HV(hv_vcpu_set_sys_reg(g->vcpu, HV_SYS_REG_TTBR0_EL1, (uintptr_t)l0));
    uint64_t sctlr = 0;
    CHECK_HV(hv_vcpu_get_sys_reg(g->vcpu, HV_SYS_REG_SCTLR_EL1, &sctlr));
    CHECK_HV(hv_vcpu_set_sys_reg(g->vcpu, HV_SYS_REG_SCTLR_EL1, sctlr | 1 | (1u << 2) | (1u << 12)));
    g->stack_size = GUEST_STACK_SIZE;
    g->stack = guest_alloc(g, g->stack_size, 16384);
    g->tls = guest_alloc(g, 65536, 16384);
    if (!g->stack || !g->tls) return -1;
    g->tls_used = 0x1000;
    g->current_locale = UINT64_MAX; // glibc LC_GLOBAL_LOCALE
    CHECK_HV(hv_vcpu_set_sys_reg(g->vcpu, HV_SYS_REG_TPIDR_EL0, (uintptr_t)g->tls));

    uint32_t *code = guest_alloc(g, 4096, 16);
    if (!code) return -1;
    // End a host-to-guest call with an HVC distinct from import stubs.
    code[0] = 0xd4000002u | (65535u << 5);
    g->return_stub = (uintptr_t)&code[0];
    // TLSDESC resolver: return the per-module offset stored in descriptor[1].
    code[1] = 0xf9400400u; // ldr x0, [x0, #8]
    code[2] = 0xd65f03c0u; // ret
    __builtin___clear_cache((char *)code,(char *)(code+3));
    g->tls_stub = (uintptr_t)&code[1];
    size_t fast_size = (size_t)(guest_fast_end - guest_fast_start);
    void *fast = guest_alloc(g, fast_size, 16);
    if (!fast) return -1;
    memcpy(fast, guest_fast_start, fast_size);
    __builtin___clear_cache(fast,(char *)fast+fast_size);
    g->fast_base = (uintptr_t)fast;
common_slots: ;
    uint64_t *slots = guest_alloc(g, 128, 16);
    if (!slots) return -1;
    memset(slots,0,128);
    slots[0] = UINT64_C(0xbadbadd00dd00d00);
    slots[1] = (uintptr_t)stdin;
    slots[2] = (uintptr_t)stdout;
    slots[3] = (uintptr_t)stderr;
    size_t environment_count=0;
    while(environ && environ[environment_count])++environment_count;
    char **guest_env=guest_alloc(g,(environment_count+1)*sizeof(char *),16);
    char **guest_argv=guest_alloc(g,2*sizeof(char *),16);
    char *arg=guest_alloc(g,sizeof("gtts-engine"),1);
    if(!guest_env || !guest_argv || !arg)return -1;
    memcpy(arg,"gtts-engine",sizeof("gtts-engine"));guest_argv[0]=arg;guest_argv[1]=NULL;g->argv=(uintptr_t)guest_argv;
    for(size_t i=0;i<environment_count;++i) {
        size_t length=strlen(environ[i])+1;guest_env[i]=guest_alloc(g,length,1);
        if(!guest_env[i])return -1;memcpy(guest_env[i],environ[i],length);
    }
    guest_env[environment_count]=NULL;slots[4]=(uintptr_t)guest_env;
    slots[5] = 0;
    g->stack_guard = (uintptr_t)&slots[0];
    g->stdin_slot = (uintptr_t)&slots[1];
    g->stdout_slot = (uintptr_t)&slots[2];
    g->stderr_slot = (uintptr_t)&slots[3];
    g->environ_slot = (uintptr_t)&slots[4];
    g->errno_slot = (uintptr_t)&slots[5];
    slots[6]=(uintptr_t)guest_alloc(g,256,16);slots[7]=(uintptr_t)guest_alloc(g,256,16);
    if(!slots[6] || !slots[7])return -1;
    g->tzname_slot=(uintptr_t)&slots[6];g->timezone_slot=(uintptr_t)&slots[8];guest_update_timezone(g);
    return 0;
}

void guest_destroy(Guest *g) {
    if(!__atomic_load_n(&g->broken,__ATOMIC_ACQUIRE))guest_run_thread_destructors(g);
    if (__atomic_load_n(&g->active_threads, __ATOMIC_ACQUIRE)) {
        // The process exit reclaims the VM after persistent engine workers stop.
        if (!g->native_mode) hv_vcpu_destroy(g->vcpu);
        return;
    }
    guest_run_exit_destructors(g, 0, __atomic_load_n(&g->broken,__ATOMIC_ACQUIRE));
    if (g->macho_mode) guest_macho_close(g);
    if (!g->native_mode) hv_vcpu_destroy(g->vcpu);
    guest_mappings_destroy(g);
    if (g->arena && g->arena != MAP_FAILED) {
        if (!g->native_mode) hv_vm_unmap((uintptr_t)g->arena, g->arena_size);
        munmap(g->arena, g->arena_size);
    }
    if (!g->native_mode) hv_vm_destroy();
    else guest_native_control_stop(g);
    free(g->native_sigactions);
    for (size_t i = 0; i < g->import_count; ++i) free(g->imports[i].name);
    for (size_t i = 0; i < g->object_count; ++i) free(g->objects[i].path);
}

void guest_thread_clone(Guest *destination, const Guest *source) {
    const Guest *shared = source->master;
    memset(destination, 0, sizeof *destination);
    destination->master = source->master;
    destination->native_mode = shared->native_mode;
    destination->native_tls_key = shared->native_tls_key;
    destination->native_guest_key = shared->native_guest_key;
    memcpy(destination->native_guest_sigmask, source->native_guest_sigmask, sizeof destination->native_guest_sigmask);
    destination->arena = shared->arena;
    destination->arena_size = shared->arena_size;
    destination->stack_size = shared->stack_size;
    destination->page_table = shared->page_table;
    destination->return_stub = shared->return_stub;
    destination->tls_stub = shared->tls_stub;
    destination->fast_base = shared->fast_base;
    destination->object_count = shared->object_count;
    destination->macho_mode = shared->macho_mode;
    memcpy(destination->macho_handles, shared->macho_handles, sizeof destination->macho_handles);
    destination->macho_shim = shared->macho_shim;
    destination->macho_enter = shared->macho_enter;
    memcpy(destination->objects, shared->objects, sizeof destination->objects);
    destination->trace = shared->trace;
    destination->verbose = shared->verbose;
    destination->profile = shared->profile;
}

int guest_thread_cpu_init(Guest *g) {
    if (g->native_mode) {
        g->stack = NULL;
        g->tls = guest_heap_aligned_alloc(g, 16384, 65536);
        g->errno_slot = (uintptr_t)guest_heap_alloc(g, 16);
        if (!g->tls || !g->errno_slot) return -1;
        memset(g->tls, 0, 65536);
        *(uint64_t *)(uintptr_t)g->errno_slot = 0;
        for (size_t i = 0; i < g->object_count; ++i) {
            ElfObject *obj = &g->objects[i];
            if (obj->tls_filesz) memcpy(g->tls + obj->tls_offset, obj->base + obj->tls_image, (size_t)obj->tls_filesz);
        }
        g->current_locale = UINT64_MAX;
        return 0;
    }
    CHECK_HV(hv_vcpu_create(&g->vcpu, &g->exit, NULL));
    g->thread_vcpu_created=1;
    CHECK_HV(hv_vcpu_set_reg(g->vcpu, HV_REG_CPSR, 0x3c5));
    CHECK_HV(hv_vcpu_set_sys_reg(g->vcpu, HV_SYS_REG_CPACR_EL1, 3u << 20));
    CHECK_HV(hv_vcpu_set_sys_reg(g->vcpu, HV_SYS_REG_MAIR_EL1, 0xff));
    uint64_t tcr = 24 | (1u << 8) | (1u << 10) | (3u << 12) |
                   (1u << 23) | (UINT64_C(2) << 32);
    CHECK_HV(hv_vcpu_set_sys_reg(g->vcpu, HV_SYS_REG_TCR_EL1, tcr));
    CHECK_HV(hv_vcpu_set_sys_reg(g->vcpu, HV_SYS_REG_TTBR0_EL1, g->page_table));
    uint64_t sctlr = 0;
    CHECK_HV(hv_vcpu_get_sys_reg(g->vcpu, HV_SYS_REG_SCTLR_EL1, &sctlr));
    CHECK_HV(hv_vcpu_set_sys_reg(g->vcpu, HV_SYS_REG_SCTLR_EL1, sctlr | 1 | (1u << 2) | (1u << 12)));
    g->stack = guest_heap_aligned_alloc(g,16384,g->stack_size);
    g->tls = guest_heap_aligned_alloc(g,16384,65536);
    g->errno_slot = (uintptr_t)guest_heap_alloc(g,16);
    if (!g->stack || !g->tls || !g->errno_slot) return -1;
    memset(g->tls,0,65536);*(uint64_t *)(uintptr_t)g->errno_slot=0;
    for (size_t i = 0; i < g->object_count; ++i) {
        ElfObject *obj = &g->objects[i];
        if (obj->tls_filesz)
            memcpy(g->tls + obj->tls_offset, obj->base + obj->tls_image, (size_t)obj->tls_filesz);
    }
    g->current_locale = UINT64_MAX;
    CHECK_HV(hv_vcpu_set_sys_reg(g->vcpu, HV_SYS_REG_TPIDR_EL0, (uintptr_t)g->tls));
    return 0;
}

uint64_t guest_import(Guest *g, const char *name) {
    Guest *shared=g->master;
    static const struct { const char *name; char *entry; } fast[] = {
        {"memcpy", guest_fast_memcpy}, {"memmove", guest_fast_memmove}, {"memset", guest_fast_memset},
        {"strlen", guest_fast_strlen}, {"strnlen", guest_fast_strnlen}, {"strcmp", guest_fast_strcmp},
        {"memcmp", guest_fast_memcmp}, {"bcmp", guest_fast_memcmp}, {"memchr", guest_fast_memchr}
    };
    if (!getenv("GTTS_NATIVE_DISABLE_FAST"))
        for (size_t i = 0; i < sizeof(fast) / sizeof(fast[0]); ++i)
            if (!strcmp(name, fast[i].name)) return shared->fast_base + (uintptr_t)(fast[i].entry - guest_fast_start);
    if (shared->native_mode && !getenv("GTTS_NATIVE_DISABLE_FAST")) {
        if (!strcmp(name, "malloc")) return (uintptr_t)guest_native_fast_malloc;
        if (!strcmp(name, "free")) return (uintptr_t)guest_native_fast_free;
    }
    pthread_mutex_lock(&shared->import_lock);
    for (size_t i = 0; i < shared->import_count; ++i)
        if (!strcmp(shared->imports[i].name, name)) {uint64_t address=shared->imports[i].address;pthread_mutex_unlock(&shared->import_lock);return address;}
    if (shared->import_count >= MAX_IMPORTS) {pthread_mutex_unlock(&shared->import_lock);return 0;}
    uint32_t *stub = shared->native_mode ? NULL : guest_alloc(g, 8, 4);
    if (!shared->native_mode && !stub) {pthread_mutex_unlock(&shared->import_lock);return 0;}
    char *copy=strdup(name);if(!copy){pthread_mutex_unlock(&shared->import_lock);return 0;}
    unsigned id = (unsigned)shared->import_count + 1;
    if (shared->native_mode) stub = (uint32_t *)(uintptr_t)guest_native_import(id);
    else {
        stub[0] = 0xd4000002u | (id << 5); // hvc #id
        stub[1] = 0xd65f03c0u; // ret
        __builtin___clear_cache((char *)stub,(char *)(stub+2));
    }
    shared->imports[shared->import_count] = (Import){.name = copy, .address = (uintptr_t)stub};
    // guest_hostcall reads the table without the lock.
    __atomic_store_n(&shared->import_count, shared->import_count + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&shared->import_lock);
    return (uintptr_t)stub;
}

static int run_guest(Guest *g, uint64_t *result) {
    for (;;) {
        CHECK_HV(hv_vcpu_run(g->vcpu));
        if (g->exit->reason == HV_EXIT_REASON_CANCELED) continue;
        uint64_t pc = 0, syndrome = g->exit->exception.syndrome;
        CHECK_HV(hv_vcpu_get_reg(g->vcpu, HV_REG_PC, &pc));
        if (g->exit->reason != HV_EXIT_REASON_EXCEPTION || (syndrome >> 26) != 0x16) {
            uint64_t lr = 0, elr = 0, esr = 0, far = 0;
            hv_vcpu_get_reg(g->vcpu, HV_REG_LR, &lr);
            hv_vcpu_get_sys_reg(g->vcpu, HV_SYS_REG_ELR_EL1, &elr);
            hv_vcpu_get_sys_reg(g->vcpu, HV_SYS_REG_ESR_EL1, &esr);
            hv_vcpu_get_sys_reg(g->vcpu, HV_SYS_REG_FAR_EL1, &far);
            fprintf(stderr, "Guest exception: reason=%u syndrome=%#llx pc=%#llx lr=%#llx va=%#llx ipa=%#llx\n",
                    (unsigned)g->exit->reason, (unsigned long long)syndrome,
                    (unsigned long long)pc, (unsigned long long)lr,
                    (unsigned long long)g->exit->exception.virtual_address,
                    (unsigned long long)g->exit->exception.physical_address);
            fprintf(stderr, "guest EL1 exception: elr=%#llx esr=%#llx far=%#llx\n",
                    (unsigned long long)elr, (unsigned long long)esr, (unsigned long long)far);
            if (pc >= g->fast_base && pc < g->fast_base + (uint64_t)(guest_fast_end - guest_fast_start))
                fprintf(stderr, "pc in fast guest code +%#llx\n", (unsigned long long)(pc - g->fast_base));
            for (size_t i = 0; i < g->object_count; ++i) {
                uint64_t base = (uintptr_t)g->objects[i].base;
                if (lr >= base && lr - base < g->objects[i].size)
                    fprintf(stderr, "lr in %s +%#llx\n", g->objects[i].path, (unsigned long long)(lr - base));
            }
            for (unsigned i = 0; i < 8; ++i) {
                uint64_t v = 0;
                hv_vcpu_get_reg(g->vcpu, (hv_reg_t)(HV_REG_X0 + i), &v);
                fprintf(stderr, "x%u=%#llx%c", i, (unsigned long long)v, i == 7 ? '\n' : ' ');
            }
            return -1;
        }
        unsigned id = (unsigned)(syndrome & 0xffffu);
        if (id == 65535) {
            if (result) CHECK_HV(hv_vcpu_get_reg(g->vcpu, HV_REG_X0, result));
            return 0;
        }
        uint64_t a[8], r = 0;
        for (unsigned i = 0; i < 8; ++i)
            CHECK_HV(hv_vcpu_get_reg(g->vcpu, (hv_reg_t)(HV_REG_X0 + i), &a[i]));
        if (guest_hostcall(g, id, a, &r) != 0) return -1;
        CHECK_HV(hv_vcpu_set_reg(g->vcpu, HV_REG_X0, r));
    }
}

int guest_call(Guest *g, uint64_t function, const uint64_t args[8], uint64_t *result) {
    if (g->native_mode) return guest_native_call(g, function, args, result);
    CHECK_HV(hv_vcpu_set_reg(g->vcpu, HV_REG_PC, function));
    CHECK_HV(hv_vcpu_set_reg(g->vcpu, HV_REG_LR, g->return_stub));
    CHECK_HV(hv_vcpu_set_sys_reg(g->vcpu, HV_SYS_REG_SP_EL1,
                                 (uintptr_t)(g->stack + g->stack_size)));
    for (unsigned i = 0; i < 8; ++i)
        CHECK_HV(hv_vcpu_set_reg(g->vcpu, (hv_reg_t)(HV_REG_X0 + i), args ? args[i] : 0));
    int status = run_guest(g, result);
    if (status) __atomic_store_n(&g->master->broken, 1, __ATOMIC_RELEASE);
    return status;
}

int guest_callback(Guest *g, uint64_t function, const uint64_t args[8], uint64_t *result) {
    if (g->native_mode) return guest_native_call(g, function, args, result);
    uint64_t saved[31], pc, cpsr, sp, fpcr, fpsr;
    hv_simd_fp_uchar16_t simd[32];
    for (unsigned i = 0; i < 31; ++i)
        CHECK_HV(hv_vcpu_get_reg(g->vcpu, (hv_reg_t)(HV_REG_X0 + i), &saved[i]));
    for (unsigned i = 0; i < 32; ++i)
        CHECK_HV(hv_vcpu_get_simd_fp_reg(g->vcpu, (hv_simd_fp_reg_t)(HV_SIMD_FP_REG_Q0 + i), &simd[i]));
    CHECK_HV(hv_vcpu_get_reg(g->vcpu, HV_REG_PC, &pc));
    CHECK_HV(hv_vcpu_get_reg(g->vcpu, HV_REG_CPSR, &cpsr));
    CHECK_HV(hv_vcpu_get_reg(g->vcpu, HV_REG_FPCR, &fpcr));
    CHECK_HV(hv_vcpu_get_reg(g->vcpu, HV_REG_FPSR, &fpsr));
    CHECK_HV(hv_vcpu_get_sys_reg(g->vcpu, HV_SYS_REG_SP_EL1, &sp));
    CHECK_HV(hv_vcpu_set_reg(g->vcpu, HV_REG_PC, function));
    CHECK_HV(hv_vcpu_set_reg(g->vcpu, HV_REG_LR, g->return_stub));
    for (unsigned i = 0; i < 8; ++i)
        CHECK_HV(hv_vcpu_set_reg(g->vcpu, (hv_reg_t)(HV_REG_X0 + i), args ? args[i] : 0));
    int status = run_guest(g, result);
    for (unsigned i = 0; i < 31; ++i)
        CHECK_HV(hv_vcpu_set_reg(g->vcpu, (hv_reg_t)(HV_REG_X0 + i), saved[i]));
    for (unsigned i = 0; i < 32; ++i)
        CHECK_HV(hv_vcpu_set_simd_fp_reg(g->vcpu, (hv_simd_fp_reg_t)(HV_SIMD_FP_REG_Q0 + i), simd[i]));
    CHECK_HV(hv_vcpu_set_reg(g->vcpu, HV_REG_PC, pc));
    CHECK_HV(hv_vcpu_set_reg(g->vcpu, HV_REG_CPSR, cpsr));
    CHECK_HV(hv_vcpu_set_reg(g->vcpu, HV_REG_FPCR, fpcr));
    CHECK_HV(hv_vcpu_set_reg(g->vcpu, HV_REG_FPSR, fpsr));
    CHECK_HV(hv_vcpu_set_sys_reg(g->vcpu, HV_SYS_REG_SP_EL1, sp));
    if (status) __atomic_store_n(&g->master->broken, 1, __ATOMIC_RELEASE);
    return status;
}
