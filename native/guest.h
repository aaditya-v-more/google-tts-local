#ifndef GTTS_GUEST_H
#define GTTS_GUEST_H

#include <Hypervisor/Hypervisor.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>

#define MAX_OBJECTS 5
#define MAX_IMPORTS 2048

typedef struct {
    char *path;
    uint8_t *base;
    size_t size;
    const char *strtab;
    void *symtab;
    size_t symbol_count;
    uint64_t rela, rela_size, jmprel, jmprel_size, relr, relr_size;
    uint64_t init, init_array, init_array_size;
    uint64_t tls_image, tls_filesz, tls_memsz, tls_align, tls_offset;
    uint64_t phdr, phnum, eh_frame, guest_path, link_map, dynamic;
    uint64_t macho_initializer;
    uint8_t *native_stubs;
    size_t native_stub_used, native_stub_size;
} ElfObject;

typedef struct {
    char *name;
    uint64_t address;
    void *host;          // host function, looked up on first call
    unsigned char kind;  // which shim family handles this import; 0 until known
} Import;

typedef struct { char name[64], gender[24]; } Speaker;

typedef struct Guest {
    struct Guest *master;
    pthread_mutex_t arena_lock, heap_lock, once_lock, import_lock, map_lock;
    pthread_cond_t once_cond;
    unsigned active_threads, next_tid, tid;
    int broken;
    int native_mode;
    int macho_mode;
    void *macho_handles[MAX_OBJECTS];
    void *macho_shim;
    void *macho_enter;
    void *native_frame;
    jmp_buf *native_abort;
    hv_simd_fp_uchar16_t native_q[8];
    uint64_t native_fpcr, native_fpsr;
    pthread_key_t native_tls_key, native_guest_key;
    pthread_t native_control_thread;
    uint32_t native_exception_port;
    int native_control_started;
    uint32_t native_alloc_lock;
    void *native_sigactions;
    uint64_t native_guest_sigmask[16];
    uint8_t *arena;
    size_t arena_size, arena_used;
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *exit;
    int thread_vcpu_created;
    uint8_t *stack;
    size_t stack_size;
    uint8_t *tls;
    size_t tls_used;
    ElfObject objects[MAX_OBJECTS];
    size_t object_count;
    Import imports[MAX_IMPORTS];
    size_t import_count;
    uint64_t return_stub, tls_stub;
    uint64_t stack_guard, stdin_slot, stdout_slot, stderr_slot, environ_slot, argv;
    uint64_t tzname_slot, timezone_slot;
    uint64_t errno_slot;
    uint64_t strerror_buffer, localeconv_buffer, localeconv_locale;
    uint64_t locale_names[16]; // names returned by setlocale
    unsigned locale_name_count;
    uint64_t default_locale, current_locale;
    uint64_t free_bins[512];
    void *mappings;
    void *thread_destructors;
    void *exit_destructors;
    uint64_t page_table;
    uint64_t fast_base;
    Speaker speakers[128];
    size_t speaker_count;
    Speaker fallback_speakers[128];
    size_t fallback_speaker_count;
    int trace;
    int verbose;
    int profile;
    uint64_t hostcall_counts[MAX_IMPORTS];
} Guest;

void *guest_alloc(Guest *g, size_t size, size_t alignment);
void *guest_heap_alloc(Guest *g, size_t size);
void *guest_heap_aligned_alloc(Guest *g, size_t alignment, size_t size);
void guest_heap_free(Guest *g, void *pointer);
int guest_create(Guest *g);
void guest_destroy(Guest *g);
int guest_call(Guest *g, uint64_t function, const uint64_t args[8], uint64_t *result);
int guest_callback(Guest *g, uint64_t function, const uint64_t args[8], uint64_t *result);
int guest_thread_cpu_init(Guest *g);
void guest_thread_clone(Guest *destination, const Guest *source);
void guest_update_timezone(Guest *g);
void guest_mappings_destroy(Guest *g);
void guest_run_thread_destructors(Guest *g);
int guest_run_exit_destructors(Guest *g, uint64_t dso, int discard);
int guest_load_objects(Guest *g);
int guest_macho_load(Guest *g);
uint64_t guest_macho_symbol(Guest *g, const char *name);
void guest_macho_close(Guest *g);
int guest_initialize_objects(Guest *g);
uint64_t guest_symbol(Guest *g, const char *name);
uint64_t guest_import(Guest *g, const char *name);
int guest_hostcall(Guest *g, unsigned id, const uint64_t args[8], uint64_t *result);
double guest_now(void);
int guest_native_available(Guest *g);
int guest_native_control_start(Guest *g);
void guest_native_control_stop(Guest *g);
int guest_native_call(Guest *g, uint64_t function, const uint64_t args[8], uint64_t *result);
uint64_t guest_native_import(unsigned id);
uint64_t guest_native_tls_stub(void);
int guest_get_simd(Guest *g, unsigned reg, hv_simd_fp_uchar16_t *value);
int guest_set_simd(Guest *g, unsigned reg, hv_simd_fp_uchar16_t value);
int guest_get_sp(Guest *g, uint64_t *value);
int guest_get_fpcr(Guest *g, uint64_t *value);
int guest_set_fpcr(Guest *g, uint64_t value);
void *guest_native_alloc_raw(Guest *g, size_t size, size_t alignment);
void *guest_native_heap_alloc(Guest *g, size_t alignment, size_t size);
void guest_native_heap_free(Guest *g, void *pointer);
void *guest_native_fast_malloc(size_t size);
void guest_native_fast_free(void *pointer);

#endif
