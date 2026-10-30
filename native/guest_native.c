#include "guest.h"
#include "guest_native.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/SecTask.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/semaphore.h>
#include <mach/exception_types.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char native_import_stubs[], native_tlsdesc[];
unsigned guest_native_tls_key, guest_native_current_key;
static pthread_once_t keys_once = PTHREAD_ONCE_INIT;
static int keys_error;
static int control_check(Guest *g);

static void make_keys(void) {
    pthread_key_t tls, current;
    if (pthread_key_create(&tls, NULL) || pthread_key_create(&current, NULL)) {
        keys_error = 1;
        return;
    }
    guest_native_tls_key = (unsigned)tls;
    guest_native_current_key = (unsigned)current;
}

static int entitlement_present(void) {
    SecTaskRef task = SecTaskCreateFromSelf(kCFAllocatorDefault);
    if (!task) return 0;
    CFTypeRef value = SecTaskCopyValueForEntitlement(task, CFSTR("com.apple.security.custom-x18-abi-toggle"), NULL);
    int present = value && CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)value);
    if (value) CFRelease(value);
    CFRelease(task);
    return present;
}

typedef struct { semaphore_t semaphore; int waiting, signal_result; } SwitchProbe;
static void *probe_competitor(void *opaque) {
    SwitchProbe *probe = opaque;
    while (!__atomic_load_n(&probe->waiting, __ATOMIC_ACQUIRE)) sched_yield();
    /* The caller must sleep in the kernel, or already have been preempted. */
    usleep(10000);
    probe->signal_result = semaphore_signal(probe->semaphore);
    return NULL;
}

int guest_native_available(Guest *g) {
    if (!__builtin_available(macOS 26.4, *)) return 0;
    if (!entitlement_present()) return 0;
    pthread_once(&keys_once, make_keys);
    if (keys_error || guest_native_tls_key >= 4096 || guest_native_current_key >= 4096) return 0;
    uintptr_t tpidrro;
    uintptr_t sentinel = UINT64_C(0x123456789abcdef0);
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tpidrro));
    const unsigned keys[] = {guest_native_tls_key, guest_native_current_key};
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        void *old = pthread_getspecific((pthread_key_t)keys[i]);
        if (pthread_setspecific((pthread_key_t)keys[i], (void *)sentinel)) return 0;
        int slot_ok = ((uintptr_t *)tpidrro)[keys[i]] == sentinel;
        pthread_setspecific((pthread_key_t)keys[i], old);
        if (!slot_ok) return 0;
    }

    /* The trap is used directly because Darwin libraries cannot run in custom
       mode. Check the system stub before relying on its trap number. */
    const uint32_t expected_trap[] = {0x92800470u, 0xd4001001u, 0xd65f03c0u};
    void *trap = dlsym(RTLD_DEFAULT, "semaphore_wait_trap");
    if (!trap || memcmp(trap, expected_trap, sizeof expected_trap)) return 0;
    SwitchProbe probe = {0};
    if (semaphore_create(mach_task_self(), &probe.semaphore, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) return 0;
    sigset_t all, oldmask;
    sigfillset(&all);
    if (pthread_sigmask(SIG_BLOCK, &all, &oldmask)) { semaphore_destroy(mach_task_self(), probe.semaphore); return 0; }
    pthread_t competitor;
    if (pthread_create(&competitor, NULL, probe_competitor, &probe)) {
        pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
        semaphore_destroy(mach_task_self(), probe.semaphore);
        return 0;
    }
    int bad = native_switch_probe(probe.semaphore, &probe.waiting);
    pthread_join(competitor, NULL);
    pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
    semaphore_destroy(mach_task_self(), probe.semaphore);
    if (bad || probe.signal_result != KERN_SUCCESS) return 0;
    g->native_tls_key = (pthread_key_t)guest_native_tls_key;
    g->native_guest_key = (pthread_key_t)guest_native_current_key;
    if (guest_native_control_start(g)) return 0;
    if (control_check(g)) { guest_native_control_stop(g); return 0; }
    return 1;
}

/* A blocked synchronous signal can otherwise repeatedly fault on macOS.
   Catch guest-thread Mach exceptions on a normal-ABI control thread and
   terminate the worker without running a signal handler in custom mode. */
static void *control_main(void *opaque) {
    Guest *g = opaque;
    sigset_t empty;
    sigemptyset(&empty);
    pthread_sigmask(SIG_SETMASK, &empty, NULL);
    union { mach_msg_header_t header; unsigned char bytes[4096]; } message;
    kern_return_t result;
    do {
        result = mach_msg(&message.header, MACH_RCV_MSG, 0, sizeof message,
                          g->native_exception_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    } while (result == MACH_RCV_INTERRUPTED);
    if (result == MACH_MSG_SUCCESS && message.header.msgh_id == 0x47545453) return NULL;
    if (result == MACH_MSG_SUCCESS) {
        static const char notice[] = "In-process engine fault; terminating worker.\n";
        (void)write(STDERR_FILENO, notice, sizeof notice - 1);
        _exit(128 + SIGSEGV);
    }
    _exit(127);
}

int guest_native_control_start(Guest *g) {
    mach_port_t port = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS) return -1;
    if (mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) {
        mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1); return -1;
    }
    g->native_exception_port = port;
    if (pthread_create(&g->native_control_thread, NULL, control_main, g)) {
        mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1);
        mach_port_deallocate(mach_task_self(), port); return -1;
    }
    g->native_control_started = 1;
    return 0;
}

void guest_native_control_stop(Guest *g) {
    if (!g->native_control_started) return;
    mach_msg_header_t stop = {0};
    stop.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    stop.msgh_size = sizeof stop;
    stop.msgh_remote_port = g->native_exception_port;
    stop.msgh_id = 0x47545453;
    if (mach_msg(&stop, MACH_SEND_MSG, sizeof stop, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) != MACH_MSG_SUCCESS)
        _exit(127);
    pthread_join(g->native_control_thread, NULL);
    mach_port_mod_refs(mach_task_self(), g->native_exception_port, MACH_PORT_RIGHT_RECEIVE, -1);
    mach_port_deallocate(mach_task_self(), g->native_exception_port);
    g->native_control_started = 0;
}

typedef struct {
    mach_port_t thread;
    exception_mask_t masks[EXC_TYPES_COUNT];
    mach_port_t ports[EXC_TYPES_COUNT];
    exception_behavior_t behaviors[EXC_TYPES_COUNT];
    thread_state_flavor_t flavors[EXC_TYPES_COUNT];
    mach_msg_type_number_t count;
} NativeExceptions;

static int install_exception_port(Guest *g, NativeExceptions *old) {
    const exception_mask_t mask = EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION |
        EXC_MASK_ARITHMETIC | EXC_MASK_BREAKPOINT | EXC_MASK_SOFTWARE;
    old->thread = mach_thread_self();
    old->count = EXC_TYPES_COUNT;
    if (thread_swap_exception_ports(old->thread, mask, g->master->native_exception_port,
                                    EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, THREAD_STATE_NONE,
                                    old->masks, &old->count, old->ports, old->behaviors, old->flavors) != KERN_SUCCESS) {
        mach_port_deallocate(mach_task_self(), old->thread); return -1;
    }
    return 0;
}

static void restore_exception_port(NativeExceptions *old) {
    for (mach_msg_type_number_t i = 0; i < old->count; ++i) {
        if (thread_set_exception_ports(old->thread, old->masks[i], old->ports[i], old->behaviors[i], old->flavors[i]) != KERN_SUCCESS)
            _exit(127);
        if (MACH_PORT_VALID(old->ports[i])) mach_port_deallocate(mach_task_self(), old->ports[i]);
    }
    mach_port_deallocate(mach_task_self(), old->thread);
}

static int control_check(Guest *g) {
    NativeExceptions old;
    if (install_exception_port(g, &old)) return -1;
    restore_exception_port(&old);
    return 0;
}

uint64_t guest_native_import(unsigned id) {
    return id && id <= MAX_IMPORTS ? (uintptr_t)native_import_stubs + (id - 1) * 8 : 0;
}
uint64_t guest_native_tls_stub(void) { return (uintptr_t)native_tlsdesc; }

void native_host_dispatch(NativeFrame *frame) {
    Guest *g = pthread_getspecific((pthread_key_t)guest_native_current_key);
    if (!g || !g->native_abort) _exit(127);
    void *previous = g->native_frame;
    g->native_frame = frame;
    uint64_t result = 0;
    int status = guest_hostcall(g, (unsigned)frame->id, frame->x, &result);
    frame->x[0] = result;
    g->native_frame = previous;
    if (status) {
        __atomic_store_n(&g->master->broken, 1, __ATOMIC_RELEASE);
        longjmp(*g->native_abort, 1);
    }
}

int guest_native_call(Guest *g, uint64_t function, const uint64_t args[8], uint64_t *result) {
    uint64_t host_fpcr, host_fpsr;
    __asm__ volatile("mrs %0, fpcr\n\tmrs %1, fpsr" : "=r"(host_fpcr), "=r"(host_fpsr));
    sigset_t all, oldmask;
    sigfillset(&all);
    if (pthread_sigmask(SIG_BLOCK, &all, &oldmask)) {
        __atomic_store_n(&g->master->broken, 1, __ATOMIC_RELEASE); return -1;
    }
    NativeExceptions exceptions;
    int installed = !g->native_abort;
    if (installed && install_exception_port(g, &exceptions)) {
        pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
        __atomic_store_n(&g->master->broken, 1, __ATOMIC_RELEASE); return -1;
    }
    void *old_guest = pthread_getspecific(g->native_guest_key);
    void *old_tls = pthread_getspecific(g->native_tls_key);
    if (pthread_setspecific(g->native_guest_key, g) || pthread_setspecific(g->native_tls_key, g->tls)) {
        pthread_setspecific(g->native_guest_key, old_guest);
        pthread_setspecific(g->native_tls_key, old_tls);
        if (installed) restore_exception_port(&exceptions);
        pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
        __atomic_store_n(&g->master->broken, 1, __ATOMIC_RELEASE);
        return -1;
    }
    NativeCall call = {0};
    if (args) memcpy(call.args, args, sizeof call.args);
    memcpy(call.q, g->native_q, sizeof call.q);
    call.fpcr = g->native_fpcr;
    call.fpsr = g->native_fpsr;
    jmp_buf env;
    jmp_buf *previous = g->native_abort;
    void *previous_frame = g->native_frame;
    g->native_abort = &env;
    int status = setjmp(env);
    if (!status) {
        /* Converted images enter through the signed shim's matching bridge. */
        void (*enter)(uint64_t, NativeCall *) = g->macho_mode && g->macho_enter ?
            (void (*)(uint64_t, NativeCall *))g->macho_enter : native_guest_enter;
        enter(function, &call);
        memcpy(g->native_q, call.q, sizeof call.q);
        g->native_fpcr = call.fpcr;
        g->native_fpsr = call.fpsr;
        if (result) *result = call.result;
    } else {
        __asm__ volatile("msr fpcr, %0\n\tmsr fpsr, %1" :: "r"(host_fpcr), "r"(host_fpsr));
    }
    g->native_abort = previous;
    g->native_frame = previous_frame;
    int restore_error = pthread_setspecific(g->native_tls_key, old_tls);
    restore_error |= pthread_setspecific(g->native_guest_key, old_guest);
    if (installed) restore_exception_port(&exceptions);
    restore_error |= pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
    if (restore_error) {
        __atomic_store_n(&g->master->broken, 1, __ATOMIC_RELEASE);
        return -1;
    }
    return status ? -1 : 0;
}

int guest_get_simd(Guest *g, unsigned reg, hv_simd_fp_uchar16_t *value) {
    if (!g->native_mode) return hv_vcpu_get_simd_fp_reg(g->vcpu, (hv_simd_fp_reg_t)(HV_SIMD_FP_REG_Q0 + reg), value) == HV_SUCCESS ? 0 : -1;
    if (reg >= 8) return -1;
    if (g->native_frame) memcpy(value, &((NativeFrame *)g->native_frame)->q[reg], sizeof *value);
    else *value = g->native_q[reg];
    return 0;
}
int guest_set_simd(Guest *g, unsigned reg, hv_simd_fp_uchar16_t value) {
    if (!g->native_mode) return hv_vcpu_set_simd_fp_reg(g->vcpu, (hv_simd_fp_reg_t)(HV_SIMD_FP_REG_Q0 + reg), value) == HV_SUCCESS ? 0 : -1;
    if (reg >= 8) return -1;
    if (g->native_frame) memcpy(&((NativeFrame *)g->native_frame)->q[reg], &value, sizeof value);
    else g->native_q[reg] = value;
    return 0;
}
int guest_get_sp(Guest *g, uint64_t *value) {
    if (!g->native_mode) return hv_vcpu_get_sys_reg(g->vcpu, HV_SYS_REG_SP_EL1, value) == HV_SUCCESS ? 0 : -1;
    if (!g->native_frame) return -1;
    *value = ((NativeFrame *)g->native_frame)->sp;
    return 0;
}
int guest_get_fpcr(Guest *g, uint64_t *value) {
    if (!g->native_mode) return hv_vcpu_get_reg(g->vcpu, HV_REG_FPCR, value) == HV_SUCCESS ? 0 : -1;
    *value = g->native_frame ? ((NativeFrame *)g->native_frame)->fpcr : g->native_fpcr;
    return 0;
}
int guest_set_fpcr(Guest *g, uint64_t value) {
    if (!g->native_mode) return hv_vcpu_set_reg(g->vcpu, HV_REG_FPCR, value) == HV_SUCCESS ? 0 : -1;
    if (g->native_frame) ((NativeFrame *)g->native_frame)->fpcr = value;
    else g->native_fpcr = value;
    return 0;
}
