#include <os/arch/arm64.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

void test_x18(uint64_t iterations, uint64_t changes[8]);
void spin_x18(uint64_t iterations, uint64_t changes[2]);
void spin_x18_pattern(uint64_t iterations, uint64_t changes[4], uint64_t pattern);
void custom_x18_yields(uint64_t iterations, uint64_t changes[4], uint64_t pattern);
void custom_x18_mach_yields(uint64_t iterations, uint64_t changes[4], uint64_t pattern);

static volatile sig_atomic_t received;
static atomic_int running;
static atomic_uint sent, send_errors;
static pthread_t target;

static void on_signal(int signal_number) {
    (void)signal_number;
    ++received;
}

static void *send_signals(void *unused) {
    (void)unused;
    const struct timespec interval = {0, 100000};
    while (atomic_load(&running)) {
        if (pthread_kill(target, SIGUSR1)) atomic_fetch_add(&send_errors, 1);
        else atomic_fetch_add(&sent, 1);
        nanosleep(&interval, NULL);
    }
    return NULL;
}

typedef struct {
    atomic_int ready, go;
} ProbeBarrier;

typedef struct {
    ProbeBarrier *barrier;
    uint64_t pattern, changes[4];
    int initial_state, enabled_state, final_state;
} ProbeThread;

// All operations between the enable and disable calls are local atomics,
// instructions in x18_test.S, or the two allowed custom-ABI API calls.
static void *custom_thread(void *arg) {
    ProbeThread *probe = arg;
    probe->initial_state = os_custom_x18_abi_enabled();
    if (!probe->initial_state) {
        os_set_custom_x18_abi_enabled(true);
        probe->enabled_state = os_custom_x18_abi_enabled();
    }
    atomic_fetch_add_explicit(&probe->barrier->ready, 1, memory_order_release);
    while (!atomic_load_explicit(&probe->barrier->go, memory_order_acquire)) {}
    if (probe->enabled_state) {
        spin_x18_pattern(UINT64_C(500000000), probe->changes, probe->pattern);
        if (os_custom_x18_abi_enabled()) os_set_custom_x18_abi_enabled(false);
    }
    probe->final_state = os_custom_x18_abi_enabled();
    return NULL;
}

static int custom_spin(uint64_t iterations, uint64_t changes[4], uint64_t pattern) {
    if (os_custom_x18_abi_enabled()) return -1;
    os_set_custom_x18_abi_enabled(true);
    if (!os_custom_x18_abi_enabled()) return -1;
    spin_x18_pattern(iterations, changes, pattern);
    // The mode may have been lost while executing the loop. Do not request a
    // second disable: the public API traps if the state is already disabled.
    if (!os_custom_x18_abi_enabled()) return -1;
    os_set_custom_x18_abi_enabled(false);
    return os_custom_x18_abi_enabled() ? -1 : 0;
}

// Measured outside this function: no time or other system call runs while the
// custom mode is enabled, apart from the two API calls themselves.
__attribute__((noinline))
static uint64_t toggle_roundtrips(uint64_t n) {
    for (uint64_t i = 0; i < n; ++i) {
        os_set_custom_x18_abi_enabled(true);
        if (!os_custom_x18_abi_enabled()) return i;
        os_set_custom_x18_abi_enabled(false);
    }
    return n;
}

static double elapsed_ns(struct timespec a, struct timespec b) {
    return (double)(b.tv_sec - a.tv_sec) * 1e9 + (double)(b.tv_nsec - a.tv_nsec);
}

static int custom_probe(void) {
    enum { THREAD_COUNT = 4 };
    const uint64_t iterations = UINT64_C(500000000);
    const uint64_t yield_iterations = 50000, toggles = 200000;
    const uint64_t pattern = UINT64_C(0x89abcdef01234567);
    sigset_t all, original, usr1;
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) || sigfillset(&all) ||
        pthread_sigmask(SIG_BLOCK, &all, &original)) {
        fprintf(stderr, "Cannot prepare probe signal handling\n");
        return 1;
    }
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);

    int failed = 0;
    uint64_t blocked[4] = {0};
    int blocked_status = custom_spin(iterations, blocked, pattern);
    printf("blocked signals: iterations=%llu changes=%llu first=%#llx lost_mode=%llu remaining=%llu status=%d\n",
           (unsigned long long)iterations, (unsigned long long)blocked[0],
           (unsigned long long)blocked[1], (unsigned long long)blocked[2],
           (unsigned long long)blocked[3], blocked_status);
    if (blocked_status || blocked[0] || blocked[2]) {
        pthread_sigmask(SIG_SETMASK, &original, NULL);
        printf("custom x18 probe: FAIL\n");
        return 1;
    }

    ProbeBarrier barrier = {0};
    ProbeThread probes[THREAD_COUNT] = {0};
    pthread_t threads[THREAD_COUNT];
    unsigned created = 0;
    for (unsigned i = 0; i < THREAD_COUNT; ++i) {
        probes[i].barrier = &barrier;
        probes[i].pattern = pattern ^ (UINT64_C(0x1111111111111111) * (i + 1));
        if (pthread_create(&threads[i], NULL, custom_thread, &probes[i])) {
            failed = 1;
            break;
        }
        ++created;
    }
    while (atomic_load_explicit(&barrier.ready, memory_order_acquire) < (int)created) {
        const struct timespec interval = {0, 100000};
        nanosleep(&interval, NULL);
    }
    int control_state = os_custom_x18_abi_enabled();
    atomic_store_explicit(&barrier.go, 1, memory_order_release);
    for (unsigned i = 0; i < created; ++i) {
        if (pthread_join(threads[i], NULL)) failed = 1;
        printf("thread %u: pattern=%#llx initial=%d enabled=%d final=%d changes=%llu first=%#llx lost_mode=%llu remaining=%llu\n",
               i, (unsigned long long)probes[i].pattern, probes[i].initial_state,
               probes[i].enabled_state, probes[i].final_state,
               (unsigned long long)probes[i].changes[0],
               (unsigned long long)probes[i].changes[1],
               (unsigned long long)probes[i].changes[2],
               (unsigned long long)probes[i].changes[3]);
        if (probes[i].initial_state || !probes[i].enabled_state ||
            probes[i].final_state || probes[i].changes[0] || probes[i].changes[2]) failed = 1;
    }
    printf("control thread custom state while workers enabled=%d\n", control_state);
    if (created != THREAD_COUNT || control_state) failed = 1;
    if (failed) {
        pthread_sigmask(SIG_SETMASK, &original, NULL);
        printf("custom x18 probe: FAIL\n");
        return 1;
    }

    uint64_t yielding[4] = {0}, mach_yielding[4] = {0};
    if (os_custom_x18_abi_enabled()) failed = 1;
    else {
        os_set_custom_x18_abi_enabled(true);
        custom_x18_mach_yields(yield_iterations, mach_yielding, pattern);
        if (!os_custom_x18_abi_enabled()) failed = 1;
        else if (mach_yielding[0] || mach_yielding[2]) failed = 1;
        else custom_x18_yields(yield_iterations, yielding, pattern);
        if (os_custom_x18_abi_enabled()) os_set_custom_x18_abi_enabled(false);
        else failed = 1;
        if (os_custom_x18_abi_enabled()) failed = 1;
    }
    printf("direct kernel swtch: calls=%llu changes=%llu first=%#llx lost_mode=%llu remaining=%llu\n",
           (unsigned long long)yield_iterations, (unsigned long long)mach_yielding[0],
           (unsigned long long)mach_yielding[1], (unsigned long long)mach_yielding[2],
           (unsigned long long)mach_yielding[3]);
    printf("sched_yield with ABI transitions: calls=%llu changes=%llu first=%#llx lost_mode=%llu remaining=%llu\n",
           (unsigned long long)yield_iterations, (unsigned long long)yielding[0],
           (unsigned long long)yielding[1], (unsigned long long)yielding[2],
           (unsigned long long)yielding[3]);
    if (yielding[0] || yielding[2] || mach_yielding[0] || mach_yielding[2]) failed = 1;
    if (failed) {
        pthread_sigmask(SIG_SETMASK, &original, NULL);
        printf("custom x18 probe: FAIL\n");
        return 1;
    }

    struct timespec start, end;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) failed = 1;
    else {
        uint64_t completed = toggle_roundtrips(toggles);
        if (completed != toggles) failed = 1;
        if (clock_gettime(CLOCK_MONOTONIC, &end)) failed = 1;
        else if (completed == toggles)
            printf("toggle round trips (including state query): calls=%llu ns_per_round_trip=%.1f\n",
                   (unsigned long long)toggles, elapsed_ns(start, end) / (double)toggles);
    }
    if (failed) {
        pthread_sigmask(SIG_SETMASK, &original, NULL);
        printf("custom x18 probe: FAIL\n");
        return 1;
    }

    uint64_t signaled[4] = {0};
    received = 0;
    atomic_store(&sent, 0);
    atomic_store(&send_errors, 0);
    target = pthread_self();
    if (pthread_sigmask(SIG_UNBLOCK, &usr1, NULL)) failed = 1;
    else {
        pthread_t sender;
        atomic_store(&running, 1);
        if (pthread_create(&sender, NULL, send_signals, NULL)) {
            atomic_store(&running, 0);
            failed = 1;
        } else {
            if (custom_spin(iterations, signaled, pattern)) failed = 1;
            atomic_store(&running, 0);
            if (pthread_join(sender, NULL)) failed = 1;
        }
        if (pthread_sigmask(SIG_BLOCK, &usr1, NULL)) failed = 1;
    }
    printf("delivered signals: sent=%u received=%d errors=%u changes=%llu first=%#llx\n",
           atomic_load(&sent), (int)received, atomic_load(&send_errors),
           (unsigned long long)signaled[0], (unsigned long long)signaled[1]);
    if (!received || atomic_load(&send_errors) || signaled[0] || signaled[2]) failed = 1;
    if (pthread_sigmask(SIG_SETMASK, &original, NULL)) failed = 1;
    printf("custom x18 probe: %s\n", failed ? "FAIL" : "PASS");
    return failed;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--custom")) {
        setvbuf(stdout, NULL, _IONBF, 0);
        if (__builtin_available(macOS 26.4, *)) return custom_probe();
        fprintf(stderr, "Custom x18 ABI requires macOS 26.4 or later\n");
        return 1;
    }
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) != 0) return 1;
    uint64_t no_signals[8] = {0};
    uint64_t spin_no_signals[2] = {0};
    const uint64_t iterations = 250000;
    test_x18(iterations, no_signals);
    spin_x18(500000000, spin_no_signals);

    target = pthread_self();
    atomic_store(&running, 1);
    pthread_t sender;
    if (pthread_create(&sender, NULL, send_signals, NULL) != 0) return 1;
    uint64_t changes[8] = {0};
    uint64_t spin_signals[2] = {0};
    test_x18(iterations, changes);
    int during_calls = received;
    spin_x18(500000000, spin_signals);
    atomic_store(&running, 0);
    pthread_join(sender, NULL);
    printf("calls=%llu; without signals: yield=%llu getpid=%llu malloc=%llu free=%llu; "
           "first yield value=%#llx\n", (unsigned long long)iterations,
           (unsigned long long)no_signals[0], (unsigned long long)no_signals[1],
           (unsigned long long)no_signals[2], (unsigned long long)no_signals[3],
           (unsigned long long)no_signals[4]);
    printf("with %d signals: yield=%llu getpid=%llu malloc=%llu free=%llu; "
           "first getpid value=%#llx\n", during_calls,
           (unsigned long long)changes[0], (unsigned long long)changes[1],
           (unsigned long long)changes[2], (unsigned long long)changes[3],
           (unsigned long long)changes[5]);
    printf("500000000-iteration pure spin: without signals changes=%llu first=%#llx; "
           "with %d signals changes=%llu first=%#llx\n",
           (unsigned long long)spin_no_signals[0], (unsigned long long)spin_no_signals[1],
           (int)received - during_calls, (unsigned long long)spin_signals[0],
           (unsigned long long)spin_signals[1]);
    return !received;
}
