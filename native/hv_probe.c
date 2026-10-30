#include <Hypervisor/Hypervisor.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

extern char guest_spin_start[], guest_spin_end[];
static volatile sig_atomic_t received;
static atomic_int running;
static pthread_t target;
static void on_signal(int n) { (void)n; ++received; }
static void *send_signals(void *unused) {
    (void)unused;
    const struct timespec interval = {0, 100000};
    while (atomic_load(&running)) {
        pthread_kill(target, SIGUSR1);
        nanosleep(&interval, NULL);
    }
    return NULL;
}

#define CHECK(call) do { \
    status = (call); \
    if (status != HV_SUCCESS) { \
        fprintf(stderr, "%s failed: %#x\n", #call, (unsigned)status); \
        return 1; \
    } \
} while (0)

int main(void) {
    uint32_t maximum = 0, default_size = 0;
    hv_vm_config_get_max_ipa_size(&maximum);
    hv_vm_config_get_default_ipa_size(&default_size);
    printf("IPA bits: maximum=%u default=%u\n", maximum, default_size);
    hv_return_t status = hv_vm_create(NULL);
    printf("hv_vm_create: %#x\n", (unsigned)status);
    if (status != HV_SUCCESS) return 1;
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *exit_info = NULL;
    status = hv_vcpu_create(&vcpu, &exit_info, NULL);
    printf("hv_vcpu_create: %#x\n", (unsigned)status);
    if (status != HV_SUCCESS) return 1;

    size_t page = (size_t)getpagesize();
    void *memory = mmap(NULL, page, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) return 1;
    memcpy(memory, guest_spin_start, (size_t)(guest_spin_end - guest_spin_start));
    CHECK(hv_vm_map(memory, 0x100000, page, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
    CHECK(hv_vcpu_set_reg(vcpu, HV_REG_PC, 0x100000));
    CHECK(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3c5)); // EL1h, interrupts masked in guest
    CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X0, 500000000));
    CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X1, UINT64_C(0x89abcdef01234567)));
    CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X18, UINT64_C(0x89abcdef01234567)));

    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) != 0) return 1;
    target = pthread_self();
    atomic_store(&running, 1);
    pthread_t sender;
    if (pthread_create(&sender, NULL, send_signals, NULL) != 0) return 1;
    do {
        CHECK(hv_vcpu_run(vcpu));
    } while (exit_info->reason == HV_EXIT_REASON_CANCELED);
    atomic_store(&running, 0);
    pthread_join(sender, NULL);
    uint64_t changes = 0, pc = 0;
    CHECK(hv_vcpu_get_reg(vcpu, HV_REG_X2, &changes));
    CHECK(hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc));
    printf("guest spin: signals=%d changes=%llu reason=%u syndrome=%#llx pc=%#llx\n",
           (int)received, (unsigned long long)changes, (unsigned)exit_info->reason,
           (unsigned long long)exit_info->exception.syndrome, (unsigned long long)pc);
    int failed = changes != 0 || !received || (exit_info->exception.syndrome >> 26) != 0x16;
    CHECK(hv_vcpu_destroy(vcpu));
    CHECK(hv_vm_destroy());
    munmap(memory, page);
    return failed;
}
