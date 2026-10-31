// The parts of glibc that the engine and libc++ use, implemented on Windows.
// Every function here is called from Linux code, so each uses the System V ABI,
// takes Linux structure layouts, and reports Linux errno values.
#include "engine.h"

#include <windows.h>
#include <bcrypt.h>
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

typedef SYSV void *(*ThreadStart)(void *);
typedef SYSV void (*Destructor)(void *);
typedef SYSV void (*VoidFunction)(void);
typedef SYSV int (*Compare)(const void *, const void *);

enum { KEY_COUNT = 128, TLS_MODULES = 8 };
enum { L_EPERM = 1, L_ENOENT = 2, L_EINTR = 4, L_EIO = 5, L_EBADF = 9, L_EAGAIN = 11, L_ENOMEM = 12,
       L_EACCES = 13, L_EEXIST = 17, L_ENOTDIR = 20, L_EISDIR = 21, L_EINVAL = 22, L_ENOSPC = 28,
       L_ERANGE = 34, L_EDEADLK = 35, L_ENAMETOOLONG = 36, L_ENOSYS = 38, L_ENOTEMPTY = 39,
       L_ETIMEDOUT = 110, L_EILSEQ = 84, L_ENOTSUP = 95 };

// ---- threads -----------------------------------------------------------------

typedef struct { Destructor fn; void *object; } ThreadDtor;
typedef struct GuestThread {
    ThreadStart start; void *arg, *result;
    HANDLE handle; DWORD id; LONG detached;
    void *keys[KEY_COUNT];
    void *tls[TLS_MODULES];
    ThreadDtor *dtors; size_t dtor_count, dtor_capacity;
    int errno_value;
    Compare sort_compare;
} GuestThread;

static DWORD thread_slot = TLS_OUT_OF_INDEXES;
static Destructor key_destructors[KEY_COUNT];
static LONG key_used[KEY_COUNT];

static GuestThread *current(void) {
    GuestThread *t = TlsGetValue(thread_slot);
    if (!t) {
        t = calloc(1, sizeof *t);
        if (!t) ExitProcess(71);
        t->id = GetCurrentThreadId();
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
        TlsSetValue(thread_slot, t);
    }
    return t;
}

static int *errno_slot(void) { return &current()->errno_value; }
static void set_errno(int linux_value) { *errno_slot() = linux_value; }
static int linux_errno(int crt) {
    switch (crt) {
    case EDEADLK: return L_EDEADLK; case ENAMETOOLONG: return L_ENAMETOOLONG; case ENOSYS: return L_ENOSYS;
    case ENOTEMPTY: return L_ENOTEMPTY; case EILSEQ: return L_EILSEQ; case ENOLCK: return 37;
    default: return crt >= 0 && crt <= 34 ? crt : L_EIO;
    }
}
static int crt_fail(void) { set_errno(linux_errno(errno)); return -1; }
static SYSV int *s_errno_location(void) { return errno_slot(); }

void *tls_block(unsigned module, uint64_t offset) {
    GuestThread *t = current();
    if (!module || module > TLS_MODULES) ExitProcess(72);
    uint8_t *block = t->tls[module - 1];
    if (!block) {
        for (unsigned i = 0; i < elf_object_count; ++i) {
            ElfObject *obj = &elf_objects[i];
            if (obj->tls_module != module) continue;
            block = _aligned_malloc(obj->tls_memsz ? (size_t)obj->tls_memsz : 1, obj->tls_align < 16 ? 16 : (size_t)obj->tls_align);
            if (!block) ExitProcess(71);
            memcpy(block, obj->base + obj->tls_image, (size_t)obj->tls_filesz);
            memset(block + obj->tls_filesz, 0, (size_t)(obj->tls_memsz - obj->tls_filesz));
        }
        if (!block) ExitProcess(72);
        t->tls[module - 1] = block;
    }
    return block + offset;
}
typedef struct { uint64_t module, offset; } TlsIndex;
static SYSV void *s_tls_get_addr(TlsIndex *index) {
    GuestThread *t = TlsGetValue(thread_slot);
    uint64_t m = index->module;
    if (t && m && m <= TLS_MODULES && t->tls[m - 1]) return (uint8_t *)t->tls[m - 1] + index->offset;
    return tls_block((unsigned)m, index->offset);
}

void shim_thread_exit(void) {
    GuestThread *t = TlsGetValue(thread_slot);
    if (!t) return;
    while (t->dtor_count) { ThreadDtor d = t->dtors[--t->dtor_count]; d.fn(d.object); }
    for (int round = 0; round < 4; ++round) {
        int again = 0;
        for (int k = 0; k < KEY_COUNT; ++k) if (t->keys[k] && key_destructors[k]) {
            void *v = t->keys[k]; t->keys[k] = NULL; key_destructors[k](v); again = 1;
        }
        if (!again) break;
    }
    for (int m = 0; m < TLS_MODULES; ++m) _aligned_free(t->tls[m]);
    memset(t->tls, 0, sizeof t->tls);
}

static DWORD WINAPI thread_entry(void *param) {
    GuestThread *t = param;
    TlsSetValue(thread_slot, t);
    t->result = t->start(t->arg);
    shim_thread_exit();
    if (InterlockedCompareExchange(&t->detached, 2, 0) == 1) { CloseHandle(t->handle); free(t->dtors); free(t); }
    return 0;
}

typedef struct { uint64_t stack_size; int detached; } ThreadAttr;   // inside pthread_attr_t (56 bytes)
static SYSV int s_pthread_attr_init(ThreadAttr *a) { memset(a, 0, 56); return 0; }
static SYSV int s_pthread_attr_destroy(ThreadAttr *a) { (void)a; return 0; }
static SYSV int s_pthread_attr_setstacksize(ThreadAttr *a, uint64_t size) { a->stack_size = size; return 0; }
static SYSV int s_pthread_attr_setdetachstate(ThreadAttr *a, int state) { a->detached = state; return 0; }
static SYSV int s_zero_int(void) { return 0; }

static SYSV int s_pthread_create(GuestThread **out, const ThreadAttr *attr, ThreadStart start, void *arg) {
    GuestThread *t = calloc(1, sizeof *t);
    if (!t) return L_EAGAIN;
    t->start = start; t->arg = arg;
    t->detached = attr && attr->detached ? 1 : 0;
    // Linux code does not probe the stack page by page, so the whole stack is
    // committed up front instead of growing behind a guard page.
    size_t stack = attr && attr->stack_size ? (size_t)attr->stack_size : 8u << 20;
    if (stack < (1u << 20)) stack = 1u << 20;
    t->handle = CreateThread(NULL, stack, thread_entry, t, CREATE_SUSPENDED, &t->id);
    if (!t->handle) { free(t); return L_EAGAIN; }
    *out = t;
    ResumeThread(t->handle);
    return 0;
}
static SYSV int s_pthread_join(GuestThread *t, void **result) {
    WaitForSingleObject(t->handle, INFINITE);
    if (result) *result = t->result;
    CloseHandle(t->handle); free(t->dtors); free(t);
    return 0;
}
static SYSV int s_pthread_detach(GuestThread *t) {
    if (InterlockedCompareExchange(&t->detached, 1, 0) == 2) { CloseHandle(t->handle); free(t->dtors); free(t); }
    return 0;
}
static SYSV GuestThread *s_pthread_self(void) { return current(); }

static SYSV int s_pthread_key_create(uint32_t *key, Destructor fn) {
    for (int k = 0; k < KEY_COUNT; ++k) if (!InterlockedCompareExchange(&key_used[k], 1, 0)) {
        key_destructors[k] = fn; *key = (uint32_t)k; return 0;
    }
    return L_EAGAIN;
}
static SYSV void *s_pthread_getspecific(uint32_t key) { return key < KEY_COUNT ? current()->keys[key] : NULL; }
static SYSV int s_pthread_setspecific(uint32_t key, const void *value) {
    if (key >= KEY_COUNT) return L_EINVAL;
    current()->keys[key] = (void *)value; return 0;
}
static SYSV int s_pthread_once(volatile LONG *once, VoidFunction fn) {
    if (*once == 2) return 0;
    if (InterlockedCompareExchange(once, 1, 0) == 0) { fn(); InterlockedExchange(once, 2); WakeByAddressAll((void *)once); return 0; }
    LONG busy = 1;
    while (*once == 1) WaitOnAddress(once, &busy, sizeof busy, INFINITE);
    return 0;
}

// pthread_mutex_t is 40 bytes: the kind sits at offset 16 as in glibc, a
// recursive owner at 8 and depth at 4, and an SRW lock in glibc's list field.
typedef struct { int lock; uint32_t count; DWORD owner; uint32_t users; int kind; int spins; SRWLOCK srw; uint64_t pad; } Mutex;
static SYSV int s_pthread_mutex_init(Mutex *m, const int *attr) { memset(m, 0, 40); if (attr) m->kind = *attr & 3; return 0; }
static SYSV int s_pthread_mutex_lock(Mutex *m) {
    if (m->kind == 1) {
        DWORD me = GetCurrentThreadId();
        if (m->owner == me) { ++m->count; return 0; }
        AcquireSRWLockExclusive(&m->srw); m->owner = me; m->count = 1; return 0;
    }
    AcquireSRWLockExclusive(&m->srw);
    if (m->kind == 2) m->owner = GetCurrentThreadId();
    return 0;
}
static SYSV int s_pthread_mutex_trylock(Mutex *m) {
    if (m->kind == 1) {
        DWORD me = GetCurrentThreadId();
        if (m->owner == me) { ++m->count; return 0; }
        if (!TryAcquireSRWLockExclusive(&m->srw)) return 16; // EBUSY
        m->owner = me; m->count = 1; return 0;
    }
    return TryAcquireSRWLockExclusive(&m->srw) ? 0 : 16;
}
static SYSV int s_pthread_mutex_unlock(Mutex *m) {
    if (m->kind == 1) {
        if (m->owner != GetCurrentThreadId()) return L_EPERM;
        if (--m->count) return 0;
        m->owner = 0;
    } else if (m->kind == 2) m->owner = 0;
    ReleaseSRWLockExclusive(&m->srw);
    return 0;
}
static SYSV int s_pthread_mutexattr_init(int *a) { *a = 0; return 0; }
static SYSV int s_pthread_mutexattr_settype(int *a, int kind) { *a = kind; return 0; }

typedef struct { CONDITION_VARIABLE cv; uint8_t pad[40]; } Cond;
static SYSV int s_pthread_cond_init(Cond *c, const void *attr) { (void)attr; memset(c, 0, 48); return 0; }
static SYSV int s_pthread_cond_signal(Cond *c) { WakeConditionVariable(&c->cv); return 0; }
static SYSV int s_pthread_cond_broadcast(Cond *c) { WakeAllConditionVariable(&c->cv); return 0; }
typedef struct { int64_t sec, nsec; } LinuxTimespec;
static DWORD wait_millis(const LinuxTimespec *abstime) {
    if (!abstime) return INFINITE;
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    int64_t now = (int64_t)((((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ull) * 100;
    int64_t until = abstime->sec * 1000000000ll + abstime->nsec;
    return until <= now ? 0 : (DWORD)min((until - now + 999999) / 1000000, (int64_t)0x7ffffffe);
}
static int cond_wait(Cond *c, Mutex *m, DWORD millis) {
    DWORD owner = m->owner; uint32_t depth = m->count;
    if (m->kind) m->owner = 0;
    BOOL ok = SleepConditionVariableSRW(&c->cv, &m->srw, millis, 0);
    if (m->kind) { m->owner = owner; m->count = depth; }
    return ok ? 0 : L_ETIMEDOUT;
}
static SYSV int s_pthread_cond_wait(Cond *c, Mutex *m) { return cond_wait(c, m, INFINITE); }
static SYSV int s_pthread_cond_timedwait(Cond *c, Mutex *m, const LinuxTimespec *t) { return cond_wait(c, m, wait_millis(t)); }

typedef struct { SRWLOCK srw; DWORD writer; } RwLock;
static SYSV int s_pthread_rwlock_rdlock(RwLock *l) { AcquireSRWLockShared(&l->srw); return 0; }
static SYSV int s_pthread_rwlock_wrlock(RwLock *l) { AcquireSRWLockExclusive(&l->srw); l->writer = GetCurrentThreadId(); return 0; }
static SYSV int s_pthread_rwlock_unlock(RwLock *l) {
    if (l->writer == GetCurrentThreadId()) { l->writer = 0; ReleaseSRWLockExclusive(&l->srw); }
    else ReleaseSRWLockShared(&l->srw);
    return 0;
}

typedef struct { volatile LONG count; } Semaphore;
static SYSV int s_sem_init(Semaphore *s, int shared, unsigned value) { (void)shared; memset(s, 0, 32); s->count = (LONG)value; return 0; }
static SYSV int s_sem_post(Semaphore *s) { InterlockedIncrement(&s->count); WakeByAddressSingle((void *)&s->count); return 0; }
static int sem_wait_for(Semaphore *s, DWORD millis) {
    ULONGLONG deadline = millis == INFINITE ? 0 : GetTickCount64() + millis;
    for (;;) {
        LONG v = s->count;
        if (v > 0) { if (InterlockedCompareExchange(&s->count, v - 1, v) == v) return 0; continue; }
        DWORD left = INFINITE;
        if (millis != INFINITE) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) { set_errno(L_ETIMEDOUT); return -1; }
            left = (DWORD)(deadline - now);
        }
        WaitOnAddress(&s->count, &v, sizeof v, left);
    }
}
static SYSV int s_sem_wait(Semaphore *s) { return sem_wait_for(s, INFINITE); }
static SYSV int s_sem_timedwait(Semaphore *s, const LinuxTimespec *t) { return sem_wait_for(s, wait_millis(t)); }

static SYSV int s_cxa_thread_atexit_impl(Destructor fn, void *object, void *dso) {
    (void)dso;
    GuestThread *t = current();
    if (t->dtor_count == t->dtor_capacity) {
        size_t n = t->dtor_capacity ? t->dtor_capacity * 2 : 16;
        ThreadDtor *p = realloc(t->dtors, n * sizeof *p);
        if (!p) return -1;
        t->dtors = p; t->dtor_capacity = n;
    }
    t->dtors[t->dtor_count++] = (ThreadDtor){fn, object};
    return 0;
}

static SYSV int s_sched_yield(void) { SwitchToThread(); return 0; }
static SYSV int s_sched_getcpu(void) { return (int)GetCurrentProcessorNumber(); }

// ---- memory --------------------------------------------------------------------

// Every block carries its real start and size in front, so free() and
// realloc() work for aligned and unaligned allocations alike.
typedef struct { void *start; size_t size; } BlockHeader;
static void *place(void *raw, size_t size, size_t align) {
    if (!raw) return NULL;
    uintptr_t p = ((uintptr_t)raw + sizeof(BlockHeader) + align - 1) & ~(uintptr_t)(align - 1);
    ((BlockHeader *)p)[-1] = (BlockHeader){raw, size};
    return (void *)p;
}
static SYSV void *s_malloc(size_t size) {
    void *p = place(malloc(size + sizeof(BlockHeader)), size, 16);
    if (!p) set_errno(L_ENOMEM);
    return p;
}
static SYSV void *s_memalign(size_t align, size_t size) {
    if (align < 16) align = 16;
    if (align & (align - 1)) { set_errno(L_EINVAL); return NULL; }
    void *p = place(malloc(size + sizeof(BlockHeader) + align), size, align);
    if (!p) set_errno(L_ENOMEM);
    return p;
}
static SYSV void s_free(void *p) { if (p) free(((BlockHeader *)p)[-1].start); }
static SYSV void *s_calloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) { set_errno(L_ENOMEM); return NULL; }
    void *p = s_malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}
static SYSV void *s_realloc(void *old, size_t size) {
    if (!old) return s_malloc(size);
    BlockHeader *h = &((BlockHeader *)old)[-1];
    if ((uint8_t *)old - (uint8_t *)h->start == sizeof(BlockHeader)) {
        void *raw = realloc(h->start, size + sizeof(BlockHeader));
        if (!raw) { set_errno(L_ENOMEM); return NULL; }
        return place(raw, size, 16);
    }
    void *p = s_malloc(size);
    if (p) { memcpy(p, old, min(size, h->size)); s_free(old); }
    return p;
}
static SYSV int s_posix_memalign(void **out, size_t align, size_t size) {
    void *p = s_memalign(align, size);
    if (!p) return align & (align - 1) ? L_EINVAL : L_ENOMEM;
    *out = p; return 0;
}
static SYSV void *s_aligned_alloc(size_t align, size_t size) { return s_memalign(align, size); }
static SYSV char *s_strdup(const char *s) {
    size_t n = strlen(s) + 1; char *p = s_malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

// Linux reserves address space lazily; here PROT_NONE mappings are only
// reserved and become committed when mprotect() opens them up.
static DWORD page_protection(int prot) {
    if (!prot) return PAGE_NOACCESS;
    if (prot & 4) return PAGE_EXECUTE_READWRITE;
    return (prot & 2) ? PAGE_READWRITE : PAGE_READONLY;
}
static void *map_memory(void *addr, size_t length, int prot, int flags) {
    DWORD type = prot ? MEM_RESERVE | MEM_COMMIT : MEM_RESERVE;
    if (flags & 0x100010) {   // MAP_FIXED or MAP_FIXED_NOREPLACE
        MEMORY_BASIC_INFORMATION info;
        if ((flags & 0x10) && VirtualQuery(addr, &info, sizeof info) && info.State != MEM_FREE) {
            if (!prot) { DWORD old; VirtualProtect(addr, length, PAGE_NOACCESS, &old); return addr; }
            void *p = VirtualAlloc(addr, length, MEM_COMMIT, page_protection(prot));
            if (p) memset(addr, 0, length);
            return p;
        }
        return VirtualAlloc(addr, length, type, page_protection(prot));
    }
    void *p = addr ? VirtualAlloc(addr, length, type, page_protection(prot)) : NULL;
    return p ? p : VirtualAlloc(NULL, length, type, prot ? page_protection(prot) : PAGE_NOACCESS);
}
static SYSV void *s_mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset) {
    if (!length) { set_errno(L_EINVAL); return (void *)-1; }
    uint8_t *p = map_memory(addr, length, (flags & 0x20) || fd < 0 ? prot : prot | 2, flags);
    if (!p) { set_errno((flags & 0x100000) ? L_EEXIST : L_ENOMEM); return (void *)-1; }
    if (!(flags & 0x20) && fd >= 0) {   // file mapping: a private copy is enough
        size_t done = 0;
        while (done < length) {
            if (_lseeki64(fd, offset + (int64_t)done, SEEK_SET) < 0) break;
            int n = _read(fd, p + done, (unsigned)min(length - done, (size_t)1 << 30));
            if (n <= 0) break;
            done += (size_t)n;
        }
    }
    return p;
}
static SYSV int s_munmap(void *addr, size_t length) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(addr, &info, sizeof info) || info.State == MEM_FREE) return 0;
    if (info.AllocationBase == addr) {
        MEMORY_BASIC_INFORMATION whole; size_t span = 0; uint8_t *q = addr;
        while (VirtualQuery(q, &whole, sizeof whole) && whole.AllocationBase == addr) { span += whole.RegionSize; q += whole.RegionSize; }
        if (span <= ((length + 4095) & ~(size_t)4095)) { VirtualFree(addr, 0, MEM_RELEASE); return 0; }
    }
    VirtualFree(addr, length, MEM_DECOMMIT);
    return 0;
}
static SYSV int s_mprotect(void *addr, size_t length, int prot) {
    if (!prot) { DWORD old; VirtualProtect(addr, length, PAGE_NOACCESS, &old); return 0; }
    if (!VirtualAlloc(addr, length, MEM_COMMIT, page_protection(prot))) { set_errno(L_ENOMEM); return -1; }
    DWORD old; VirtualProtect(addr, length, page_protection(prot), &old);
    return 0;
}
static SYSV void *s_sbrk(intptr_t n) { (void)n; set_errno(L_ENOMEM); return (void *)-1; }

// ---- files ----------------------------------------------------------------------

wchar_t *utf8_to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof(wchar_t)) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
char *wide_to_utf8(const wchar_t *s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *u = n > 0 ? malloc((size_t)n) : NULL;
    if (u) WideCharToMultiByte(CP_UTF8, 0, s, -1, u, n, NULL, NULL);
    return u;
}

enum { URANDOM_FD = 0x7ff0, VIRTUAL_FD = 0x7f00, VIRTUAL_COUNT = 64 };

// A few Linux system files the engine reads, served from memory.
typedef struct { char *text; size_t size, position; } VirtualFile;
static VirtualFile virtual_files[VIRTUAL_COUNT];
static SRWLOCK virtual_lock = SRWLOCK_INIT;
static int is_virtual(int fd) { return fd >= VIRTUAL_FD && fd < VIRTUAL_FD + VIRTUAL_COUNT && virtual_files[fd - VIRTUAL_FD].text; }
static int open_virtual(const char *path) {
    TextBuffer text = {0};
    SYSTEM_INFO info; GetSystemInfo(&info);
    unsigned cpus = info.dwNumberOfProcessors;
    char line[160];
    if (!strcmp(path, "/sys/devices/system/cpu/possible") || !strcmp(path, "/sys/devices/system/cpu/online") ||
        !strcmp(path, "/sys/devices/system/cpu/present")) {
        snprintf(line, sizeof line, "0-%u\n", cpus - 1);
        text.data = _strdup(line);
    } else if (!strcmp(path, "/sys/devices/system/cpu/kernel_max")) {
        snprintf(line, sizeof line, "%u\n", cpus - 1);
        text.data = _strdup(line);
    } else if (!strcmp(path, "/proc/cpuinfo")) {
        // XNNPACK's CPU detection counts processors and their APIC ids here.
        size_t cap = (size_t)cpus * 160 + 1, used = 0;
        text.data = malloc(cap);
        for (unsigned i = 0; text.data && i < cpus; ++i)
            used += (size_t)snprintf(text.data + used, cap - used,
                                     "processor\t: %u\nphysical id\t: 0\nsiblings\t: %u\ncore id\t\t: %u\n"
                                     "apicid\t\t: %u\ninitial apicid\t: %u\n\n", i, cpus, i / 2, i, i);
    } else return -2;
    if (!text.data) return -1;
    AcquireSRWLockExclusive(&virtual_lock);
    for (int i = 0; i < VIRTUAL_COUNT; ++i) if (!virtual_files[i].text) {
        virtual_files[i].text = text.data; virtual_files[i].size = strlen(text.data); virtual_files[i].position = 0;
        ReleaseSRWLockExclusive(&virtual_lock);
        return VIRTUAL_FD + i;
    }
    ReleaseSRWLockExclusive(&virtual_lock);
    free(text.data);
    return -1;
}

static SYSV int s_open(const char *path, int flags, int mode) {
    if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random")) return URANDOM_FD;
    if (!strncmp(path, "/sys/", 5) || !strncmp(path, "/proc/", 6)) {
        int fd = open_virtual(path);
        if (fd == -2) {
            if (getenv("GTTS_TRACE")) fprintf(stderr, "no virtual file for %s\n", path);
            set_errno(L_ENOENT); return -1;
        }
        if (fd < 0) set_errno(L_EAGAIN);
        return fd;
    }
    int f = _O_BINARY | _O_NOINHERIT;
    switch (flags & 3) { case 1: f |= _O_WRONLY; break; case 2: f |= _O_RDWR; break; default: f |= _O_RDONLY; }
    if (flags & 0100) f |= _O_CREAT;
    if (flags & 0200) f |= _O_EXCL;
    if (flags & 01000) f |= _O_TRUNC;
    if (flags & 02000) f |= _O_APPEND;
    wchar_t *w = utf8_to_wide(path);
    if (!w) { set_errno(L_ENOMEM); return -1; }
    int fd = _wopen(w, f, (mode & 0200) ? _S_IREAD | _S_IWRITE : _S_IREAD);
    free(w);
    return fd < 0 ? crt_fail() : fd;
}
static SYSV int s_open_2(const char *path, int flags) { return s_open(path, flags, 0666); }
static SYSV int s_openat(int dirfd, const char *path, int flags, int mode) {
    if (dirfd != -100 && path[0] != '/' && !(path[0] && path[1] == ':')) { set_errno(L_ENOSYS); return -1; }
    return s_open(path, flags, mode);
}
static SYSV int s_close(int fd) {
    if (fd == URANDOM_FD) return 0;
    if (is_virtual(fd)) {
        AcquireSRWLockExclusive(&virtual_lock);
        free(virtual_files[fd - VIRTUAL_FD].text); virtual_files[fd - VIRTUAL_FD].text = NULL;
        ReleaseSRWLockExclusive(&virtual_lock);
        return 0;
    }
    return _close(fd) ? crt_fail() : 0;
}
static SYSV int64_t s_read(int fd, void *buf, size_t n) {
    if (fd == URANDOM_FD) { BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG); return (int64_t)n; }
    if (is_virtual(fd)) {
        VirtualFile *v = &virtual_files[fd - VIRTUAL_FD];
        size_t k = min(n, v->size - v->position);
        memcpy(buf, v->text + v->position, k); v->position += k;
        return (int64_t)k;
    }
    int r = _read(fd, buf, (unsigned)min(n, (size_t)INT_MAX));
    return r < 0 ? crt_fail() : r;
}
static SYSV int64_t s_read_chk(int fd, void *buf, size_t n, size_t buflen) { (void)buflen; return s_read(fd, buf, n); }
static SYSV int64_t s_write(int fd, const void *buf, size_t n) {
    if (fd == 1) fd = 2;   // stdout carries the worker protocol
    int r = _write(fd, buf, (unsigned)min(n, (size_t)INT_MAX));
    return r < 0 ? crt_fail() : r;
}
static SYSV int64_t s_lseek(int fd, int64_t offset, int whence) {
    int64_t r = _lseeki64(fd, offset, whence);
    return r < 0 ? crt_fail() : r;
}
static SYSV int64_t s_pread(int fd, void *buf, size_t n, int64_t offset) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { set_errno(L_EBADF); return -1; }
    OVERLAPPED o = {0}; o.Offset = (DWORD)offset; o.OffsetHigh = (DWORD)(offset >> 32);
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)min(n, (size_t)0x7fffffff), &got, &o)) {
        if (GetLastError() == ERROR_HANDLE_EOF) return 0;
        set_errno(L_EIO); return -1;
    }
    return got;
}
static SYSV int64_t s_pread_chk(int fd, void *buf, size_t n, int64_t offset, size_t buflen) { (void)buflen; return s_pread(fd, buf, n, offset); }
static SYSV int s_dup(int fd) { int r = _dup(fd); return r < 0 ? crt_fail() : r; }
static SYSV int s_ftruncate(int fd, int64_t size) { return _chsize_s(fd, size) ? (set_errno(L_EIO), -1) : 0; }
static SYSV int s_fallocate(int fd, int mode, int64_t offset, int64_t len) {
    (void)mode;
    int64_t end = offset + len, size = _filelengthi64(fd);
    return size < end ? s_ftruncate(fd, end) : 0;
}
static SYSV int s_isatty(int fd) { return _isatty(fd) ? 1 : (set_errno(25), 0); }

// struct stat on x86-64 Linux, 144 bytes.
typedef struct {
    uint64_t dev, ino, nlink; uint32_t mode, uid, gid, pad0; uint64_t rdev; int64_t size, blksize, blocks;
    LinuxTimespec atime, mtime, ctime; int64_t reserved[3];
} LinuxStat;
static void fill_stat(LinuxStat *out, const struct _stat64 *st) {
    memset(out, 0, sizeof *out);
    out->dev = (uint64_t)st->st_dev; out->ino = st->st_ino; out->nlink = (uint64_t)st->st_nlink;
    out->mode = (uint32_t)st->st_mode; out->size = st->st_size; out->blksize = 4096; out->blocks = (st->st_size + 511) / 512;
    out->atime.sec = st->st_atime; out->mtime.sec = st->st_mtime; out->ctime.sec = st->st_ctime;
}
static SYSV int s_stat(const char *path, LinuxStat *out) {
    wchar_t *w = utf8_to_wide(path);
    struct _stat64 st;
    int r = w ? _wstat64(w, &st) : -1;
    free(w);
    if (r) return crt_fail();
    fill_stat(out, &st); return 0;
}
static SYSV int s_fstat(int fd, LinuxStat *out) {
    if (fd == URANDOM_FD) { memset(out, 0, sizeof *out); out->mode = 020666; return 0; }
    if (is_virtual(fd)) { memset(out, 0, sizeof *out); out->mode = 0100444; return 0; }
    struct _stat64 st;
    if (_fstat64(fd, &st)) return crt_fail();
    fill_stat(out, &st); return 0;
}
static SYSV int s_access(const char *path, int mode) {
    wchar_t *w = utf8_to_wide(path);
    int r = w ? _waccess(w, mode & 6) : -1;
    free(w);
    return r ? crt_fail() : 0;
}
static SYSV int s_mkdir(const char *path, unsigned mode) {
    (void)mode;
    wchar_t *w = utf8_to_wide(path);
    int r = w ? _wmkdir(w) : -1;
    free(w);
    return r ? crt_fail() : 0;
}
static SYSV int s_remove(const char *path) {
    wchar_t *w = utf8_to_wide(path);
    int r = w ? (_wremove(w) && _wrmdir(w)) : -1;
    free(w);
    return r ? crt_fail() : 0;
}
static SYSV int s_unlinkat(int dirfd, const char *path, int flags) { (void)dirfd; (void)flags; return s_remove(path); }
static SYSV int s_rename(const char *from, const char *to) {
    wchar_t *a = utf8_to_wide(from), *b = utf8_to_wide(to);
    BOOL ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
    free(a); free(b);
    if (!ok) { set_errno(L_EACCES); return -1; }
    return 0;
}
static SYSV char *s_getcwd(char *buf, size_t size) {
    wchar_t w[MAX_PATH * 2];
    if (!_wgetcwd(w, MAX_PATH * 2)) { crt_fail(); return NULL; }
    char *u = wide_to_utf8(w);
    if (!u) return NULL;
    for (char *p = u; *p; ++p) if (*p == '\\') *p = '/';
    if (!buf) return u;
    if (strlen(u) + 1 > size) { free(u); set_errno(L_ERANGE); return NULL; }
    strcpy(buf, u); free(u); return buf;
}
static SYSV char *s_realpath(const char *path, char *resolved) {
    wchar_t *w = utf8_to_wide(path), full[MAX_PATH * 2];
    if (!w || !_wfullpath(full, w, MAX_PATH * 2) || _waccess(full, 0)) { free(w); set_errno(L_ENOENT); return NULL; }
    free(w);
    char *u = wide_to_utf8(full);
    if (!u) return NULL;
    for (char *p = u; *p; ++p) if (*p == '\\') *p = '/';
    if (!resolved) { char *g = s_strdup(u); free(u); return g; }
    snprintf(resolved, 4096, "%s", u); free(u);
    return resolved;
}
static SYSV char *s_realpath_chk(const char *path, char *resolved, size_t n) { (void)n; return s_realpath(path, resolved); }
static SYSV int64_t s_readlink(const char *path, char *buf, size_t n) { (void)path; (void)buf; (void)n; set_errno(L_EINVAL); return -1; }
static SYSV int64_t s_readlink_chk(const char *path, char *buf, size_t n, size_t m) { (void)m; return s_readlink(path, buf, n); }

typedef struct { HANDLE find; WIN32_FIND_DATAW data; int first; uint8_t entry[280]; } Directory;
static SYSV Directory *s_opendir(const char *path) {
    size_t n = strlen(path);
    char *pattern = malloc(n + 3);
    if (!pattern) return NULL;
    memcpy(pattern, path, n);
    strcpy(pattern + n, n && (path[n - 1] == '/' || path[n - 1] == '\\') ? "*" : "/*");
    wchar_t *w = utf8_to_wide(pattern);
    free(pattern);
    Directory *d = calloc(1, sizeof *d);
    if (!w || !d) { free(w); free(d); set_errno(L_ENOMEM); return NULL; }
    d->find = FindFirstFileW(w, &d->data);
    free(w);
    if (d->find == INVALID_HANDLE_VALUE) { free(d); set_errno(L_ENOENT); return NULL; }
    d->first = 1;
    return d;
}
static SYSV void *s_readdir(Directory *d) {
    if (!d->first && !FindNextFileW(d->find, &d->data)) return NULL;
    d->first = 0;
    // struct dirent64: d_ino, d_off, d_reclen, d_type, d_name[256]
    memset(d->entry, 0, sizeof d->entry);
    uint64_t ino = 1; memcpy(d->entry, &ino, 8);
    uint16_t reclen = 280; memcpy(d->entry + 16, &reclen, 2);
    d->entry[18] = (d->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 4 : 8;
    WideCharToMultiByte(CP_UTF8, 0, d->data.cFileName, -1, (char *)d->entry + 19, 256, NULL, NULL);
    return d->entry;
}
static SYSV int s_closedir(Directory *d) { FindClose(d->find); free(d); return 0; }

// ---- stdio ----------------------------------------------------------------------

static FILE *guest_stdin, *guest_stdout, *guest_stderr;
static FILE *guest_stdin_slot, *guest_stdout_slot, *guest_stderr_slot;

static SYSV FILE *s_fopen(const char *path, const char *mode) {
    wchar_t wmode[16]; size_t j = 0;
    for (const char *m = mode; *m && j < 12; ++m) if (*m != 'b' && *m != 'e' && *m != 'm' && *m != 'x') wmode[j++] = (wchar_t)*m;
    wmode[j++] = L'b'; wmode[j++] = L'N'; wmode[j] = 0;
    wchar_t *w = utf8_to_wide(path);
    FILE *f = w ? _wfopen(w, wmode) : NULL;
    free(w);
    if (!f) crt_fail();
    return f;
}
static SYSV int s_fclose(FILE *f) { return fclose(f) ? crt_fail() : 0; }
static SYSV size_t s_fread(void *p, size_t size, size_t n, FILE *f) { return fread(p, size, n, f); }
static SYSV size_t s_fread_chk(void *p, size_t buflen, size_t size, size_t n, FILE *f) { (void)buflen; return fread(p, size, n, f); }
static SYSV size_t s_fwrite(const void *p, size_t size, size_t n, FILE *f) { return fwrite(p, size, n, f); }
static SYSV int s_fflush(FILE *f) { return fflush(f); }
static SYSV int s_feof(FILE *f) { return feof(f); }
static SYSV int s_fgetc(FILE *f) { return fgetc(f); }
static SYSV int s_ungetc(int c, FILE *f) { return ungetc(c, f); }
static SYSV char *s_fgets(char *s, int n, FILE *f) { return fgets(s, n, f); }
static SYSV char *s_fgets_chk(char *s, size_t len, int n, FILE *f) { (void)len; return fgets(s, n, f); }
static SYSV int s_fputc(int c, FILE *f) { return fputc(c, f); }
static SYSV int s_fseek(FILE *f, int64_t off, int whence) { return _fseeki64(f, off, whence) ? crt_fail() : 0; }
static SYSV int64_t s_ftell(FILE *f) { return _ftelli64(f); }
static SYSV void s_rewind(FILE *f) { rewind(f); }
static SYSV int s_fileno(FILE *f) { return _fileno(f); }
static SYSV FILE *s_fdopen(int fd, const char *mode) { (void)mode; return _fdopen(fd, "rb"); }

static int write_buffer(FILE *f, TextBuffer *b, int n) {
    if (n >= 0 && b->size && fwrite(b->data, 1, b->size, f) != b->size) n = -1;
    free(b->data);
    return n;
}
static SYSV int s_vfprintf_chk(FILE *f, int flag, const char *fmt, SysvVaList *ap) {
    (void)flag; TextBuffer b = {0};
    return write_buffer(f, &b, sysv_format(&b, fmt, ap));
}
static SYSV int s_vprintf_chk(int flag, const char *fmt, SysvVaList *ap) { return s_vfprintf_chk(guest_stdout, flag, fmt, ap); }
__attribute__((noinline)) static SYSV int s_fprintf(FILE *f, const char *fmt, ...) {
    __builtin_sysv_va_list ap; __builtin_sysv_va_start(ap, fmt);
    TextBuffer b = {0};
    int n = write_buffer(f, &b, sysv_format(&b, fmt, (SysvVaList *)ap));
    __builtin_sysv_va_end(ap); return n;
}
__attribute__((noinline)) static SYSV int s_fprintf_chk(FILE *f, int flag, const char *fmt, ...) {
    (void)flag;
    __builtin_sysv_va_list ap; __builtin_sysv_va_start(ap, fmt);
    TextBuffer b = {0};
    int n = write_buffer(f, &b, sysv_format(&b, fmt, (SysvVaList *)ap));
    __builtin_sysv_va_end(ap); return n;
}
static int copy_out(char *s, size_t max, TextBuffer *b, int n) {
    if (n >= 0 && max) { size_t k = min(b->size, max - 1); memcpy(s, b->data, k); s[k] = 0; }
    free(b->data);
    return n;
}
static SYSV int s_vsnprintf_chk(char *s, size_t max, int flag, size_t slen, const char *fmt, SysvVaList *ap) {
    (void)flag; (void)slen; TextBuffer b = {0};
    return copy_out(s, max, &b, sysv_format(&b, fmt, ap));
}
static SYSV int s_vsprintf_chk(char *s, int flag, size_t slen, const char *fmt, SysvVaList *ap) {
    (void)flag; TextBuffer b = {0};
    return copy_out(s, slen, &b, sysv_format(&b, fmt, ap));
}
__attribute__((noinline)) static SYSV int s_snprintf(char *s, size_t max, const char *fmt, ...) {
    __builtin_sysv_va_list ap; __builtin_sysv_va_start(ap, fmt);
    TextBuffer b = {0};
    int n = copy_out(s, max, &b, sysv_format(&b, fmt, (SysvVaList *)ap));
    __builtin_sysv_va_end(ap); return n;
}
static SYSV int s_vasprintf_chk(char **out, int flag, const char *fmt, SysvVaList *ap) {
    (void)flag; TextBuffer b = {0};
    int n = sysv_format(&b, fmt, ap);
    if (n < 0) { free(b.data); return -1; }
    *out = s_malloc(b.size + 1);
    if (!*out) { free(b.data); return -1; }
    memcpy(*out, b.data ? b.data : "", b.size); (*out)[b.size] = 0;
    free(b.data);
    return n;
}
__attribute__((noinline)) static SYSV int s_swprintf(uint32_t *s, size_t max, const uint32_t *fmt, ...) {
    __builtin_sysv_va_list ap; __builtin_sysv_va_start(ap, fmt);
    TextBuffer b = {0};
    int n = sysv_wformat(&b, fmt, (SysvVaList *)ap);
    __builtin_sysv_va_end(ap);
    size_t count = b.size / 4;
    if (n < 0 || !max || count >= max) { if (max) s[0] = 0; free(b.data); return -1; }
    memcpy(s, b.data, b.size); s[count] = 0;
    free(b.data);
    return (int)count;
}
__attribute__((noinline)) static SYSV int s_sscanf(const char *input, const char *fmt, ...) {
    __builtin_sysv_va_list ap; __builtin_sysv_va_start(ap, fmt);
    int n = sysv_scan(input, fmt, (SysvVaList *)ap);
    __builtin_sysv_va_end(ap); return n;
}
static SYSV int s_vsscanf(const char *input, const char *fmt, SysvVaList *ap) { return sysv_scan(input, fmt, ap); }

int guest_fprintf_stderr(const char *format, ...) {
    va_list ap; va_start(ap, format);
    int n = vfprintf(stderr, format, ap);
    va_end(ap); return n;
}

// ---- strings ---------------------------------------------------------------------

static SYSV void *s_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static SYSV void *s_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static SYSV void *s_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static SYSV void *s_memcpy_chk(void *d, const void *s, size_t n, size_t m) { (void)m; return memcpy(d, s, n); }
static SYSV void *s_memmove_chk(void *d, const void *s, size_t n, size_t m) { (void)m; return memmove(d, s, n); }
static SYSV void *s_memset_chk(void *d, int c, size_t n, size_t m) { (void)m; return memset(d, c, n); }
static SYSV int s_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static SYSV void *s_memchr(const void *s, int c, size_t n) { return memchr(s, c, n); }
static SYSV size_t s_strlen(const char *s) { return strlen(s); }
static SYSV size_t s_strnlen(const char *s, size_t n) { return strnlen(s, n); }
static SYSV int s_strcmp(const char *a, const char *b) { return strcmp(a, b); }
static SYSV int s_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
static SYSV int s_strcoll_l(const char *a, const char *b, void *l) { (void)l; return strcmp(a, b); }
static SYSV size_t s_strxfrm_l(char *d, const char *s, size_t n, void *l) {
    (void)l; size_t len = strlen(s);
    if (n) { size_t k = min(len, n - 1); memcpy(d, s, k); d[k] = 0; }
    return len;
}
static SYSV char *s_strcpy(char *d, const char *s) { return strcpy(d, s); }
static SYSV char *s_strncpy(char *d, const char *s, size_t n) { return strncpy(d, s, n); }
static SYSV char *s_strcat(char *d, const char *s) { return strcat(d, s); }
static SYSV char *s_strncat(char *d, const char *s, size_t n) { return strncat(d, s, n); }
static SYSV char *s_strcpy_chk(char *d, const char *s, size_t m) { (void)m; return strcpy(d, s); }
static SYSV char *s_strncpy_chk(char *d, const char *s, size_t n, size_t m) { (void)m; return strncpy(d, s, n); }
static SYSV char *s_strcat_chk(char *d, const char *s, size_t m) { (void)m; return strcat(d, s); }
static SYSV char *s_strncat_chk(char *d, const char *s, size_t n, size_t m) { (void)m; return strncat(d, s, n); }
static SYSV char *s_strchr(const char *s, int c) { return strchr(s, c); }
static SYSV char *s_strrchr(const char *s, int c) { return strrchr(s, c); }
static SYSV char *s_strstr(const char *a, const char *b) { return strstr(a, b); }

static const char *error_text(int e) {
    switch (e) {
    case 0: return "Success"; case L_EPERM: return "Operation not permitted"; case L_ENOENT: return "No such file or directory";
    case L_EINTR: return "Interrupted system call"; case L_EIO: return "Input/output error"; case L_EBADF: return "Bad file descriptor";
    case L_EAGAIN: return "Resource temporarily unavailable"; case L_ENOMEM: return "Cannot allocate memory";
    case L_EACCES: return "Permission denied"; case L_EEXIST: return "File exists"; case L_ENOTDIR: return "Not a directory";
    case L_EISDIR: return "Is a directory"; case L_EINVAL: return "Invalid argument"; case L_ENOSPC: return "No space left on device";
    case L_ERANGE: return "Numerical result out of range"; case L_ENOSYS: return "Function not implemented";
    case L_ETIMEDOUT: return "Connection timed out"; default: return "Unknown error";
    }
}
static SYSV char *s_strerror(int e) { return (char *)error_text(e); }
static SYSV char *s_strerror_r(int e, char *buf, size_t n) { (void)buf; (void)n; return (char *)error_text(e); }

static SYSV int64_t s_strtol(const char *s, char **end, int base) { errno = 0; int64_t v = _strtoi64(s, end, base); if (errno) set_errno(L_ERANGE); return v; }
static SYSV uint64_t s_strtoul(const char *s, char **end, int base) { errno = 0; uint64_t v = _strtoui64(s, end, base); if (errno) set_errno(L_ERANGE); return v; }
static SYSV int64_t s_strtoll_l(const char *s, char **end, int base, void *l) { (void)l; return s_strtol(s, end, base); }
static SYSV uint64_t s_strtoull_l(const char *s, char **end, int base, void *l) { (void)l; return s_strtoul(s, end, base); }
static SYSV double s_strtod(const char *s, char **end) { errno = 0; double v = __mingw_strtod(s, end); if (errno) set_errno(L_ERANGE); return v; }
static SYSV float s_strtof(const char *s, char **end) { errno = 0; float v = __mingw_strtof(s, end); if (errno) set_errno(L_ERANGE); return v; }
static SYSV long double s_strtold(const char *s, char **end) { errno = 0; long double v = __mingw_strtold(s, end); if (errno) set_errno(L_ERANGE); return v; }
static SYSV double s_strtod_l(const char *s, char **end, void *l) { (void)l; return s_strtod(s, end); }
static SYSV float s_strtof_l(const char *s, char **end, void *l) { (void)l; return s_strtof(s, end); }
static SYSV long double s_strtold_l(const char *s, char **end, void *l) { (void)l; return s_strtold(s, end); }

// Wide characters are 32 bits on Linux. In the C locale each byte is one character.
static SYSV size_t s_wcslen(const uint32_t *s) { size_t n = 0; while (s[n]) ++n; return n; }
static SYSV uint32_t *s_wmemchr(const uint32_t *s, uint32_t c, size_t n) { for (size_t i = 0; i < n; ++i) if (s[i] == c) return (uint32_t *)s + i; return NULL; }
static SYSV int s_wmemcmp(const uint32_t *a, const uint32_t *b, size_t n) { for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1; return 0; }
static SYSV uint32_t s_btowc(int c) { return c == EOF ? 0xffffffffu : (uint32_t)(unsigned char)c; }
static SYSV int s_wctob(uint32_t c) { return c < 256 ? (int)c : EOF; }
static SYSV size_t s_mbrtowc(uint32_t *pwc, const char *s, size_t n, void *state) {
    (void)state;
    if (!s) return 0;
    if (!n) return (size_t)-2;
    if (pwc) *pwc = (unsigned char)*s;
    return *s ? 1 : 0;
}
static SYSV int s_mbtowc(uint32_t *pwc, const char *s, size_t n) { return s ? (int)s_mbrtowc(pwc, s, n, NULL) : 0; }
static SYSV size_t s_wcrtomb(char *s, uint32_t wc, void *state) {
    (void)state;
    if (!s) return 1;
    if (wc > 255) { set_errno(L_EILSEQ); return (size_t)-1; }
    *s = (char)wc; return 1;
}
static SYSV size_t s_mbsnrtowcs(uint32_t *dst, const char **src, size_t nms, size_t len, void *state) {
    (void)state; size_t i = 0; const char *s = *src;
    for (; i < nms && (!dst || i < len); ++i) {
        if (dst) dst[i] = (unsigned char)s[i];
        if (!s[i]) { if (dst) *src = NULL; return i; }
    }
    if (dst) *src = s + i;
    return i;
}
static SYSV size_t s_mbsrtowcs(uint32_t *dst, const char **src, size_t len, void *state) { return s_mbsnrtowcs(dst, src, SIZE_MAX, len, state); }
static SYSV size_t s_wcsnrtombs(char *dst, const uint32_t **src, size_t nwc, size_t len, void *state) {
    (void)state; size_t i = 0; const uint32_t *s = *src;
    for (; i < nwc && (!dst || i < len); ++i) {
        if (s[i] > 255) { set_errno(L_EILSEQ); return (size_t)-1; }
        if (dst) dst[i] = (char)s[i];
        if (!s[i]) { if (dst) *src = NULL; return i; }
    }
    if (dst) *src = s + i;
    return i;
}
static SYSV size_t s_mb_cur_max(void) { return 1; }

// ---- ctype and locale ---------------------------------------------------------------

static uint16_t ctype_table[384];
static int32_t lower_table[384], upper_table[384];
static const uint16_t *ctype_pointer = ctype_table + 128;
static const int32_t *lower_pointer = lower_table + 128, *upper_pointer = upper_table + 128;

// glibc's bit order for the classification table on a little-endian machine.
enum { C_UPPER = 0x100, C_LOWER = 0x200, C_ALPHA = 0x400, C_DIGIT = 0x800, C_XDIGIT = 0x1000, C_SPACE = 0x2000,
       C_PRINT = 0x4000, C_GRAPH = 0x8000, C_BLANK = 1, C_CNTRL = 2, C_PUNCT = 4, C_ALNUM = 8 };

static void init_ctype(void) {
    for (int i = -128; i < 256; ++i) {
        int c = i < 0 ? i + 256 : i;
        uint16_t m = 0;
        if (c < 128) {
            if (c >= 'A' && c <= 'Z') m |= C_UPPER | C_ALPHA | C_ALNUM;
            if (c >= 'a' && c <= 'z') m |= C_LOWER | C_ALPHA | C_ALNUM;
            if (c >= '0' && c <= '9') m |= C_DIGIT | C_ALNUM | C_XDIGIT;
            if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) m |= C_XDIGIT;
            if (c == ' ' || (c >= 9 && c <= 13)) m |= C_SPACE;
            if (c == ' ' || c == '\t') m |= C_BLANK;
            if (c < 32 || c == 127) m |= C_CNTRL;
            if (c >= 32 && c < 127) m |= C_PRINT;
            if (c > 32 && c < 127) m |= C_GRAPH;
            if (c > 32 && c < 127 && !(m & C_ALNUM)) m |= C_PUNCT;
        }
        ctype_table[i + 128] = i == -1 ? 0 : m;
        lower_table[i + 128] = i >= 0 && c >= 'A' && c <= 'Z' ? c + 32 : i;
        upper_table[i + 128] = i >= 0 && c >= 'a' && c <= 'z' ? c - 32 : i;
    }
}
static SYSV const uint16_t **s_ctype_b_loc(void) { return &ctype_pointer; }
static SYSV const int32_t **s_ctype_tolower_loc(void) { return &lower_pointer; }
static SYSV const int32_t **s_ctype_toupper_loc(void) { return &upper_pointer; }
static SYSV int s_tolower_l(int c, void *l) { (void)l; return c >= -128 && c < 256 ? lower_table[c + 128] : c; }
static SYSV int s_toupper_l(int c, void *l) { (void)l; return c >= -128 && c < 256 ? upper_table[c + 128] : c; }
static uint16_t wide_class(uint32_t c) { return c < 128 ? ctype_table[c + 128] : 0; }
#define WCLASS(name, mask) static SYSV int s_##name(uint32_t c, void *l) { (void)l; return (wide_class(c) & (mask)) != 0; }
WCLASS(iswalpha_l, C_ALPHA) WCLASS(iswblank_l, C_BLANK) WCLASS(iswcntrl_l, C_CNTRL) WCLASS(iswdigit_l, C_DIGIT)
WCLASS(iswlower_l, C_LOWER) WCLASS(iswprint_l, C_PRINT) WCLASS(iswpunct_l, C_PUNCT) WCLASS(iswspace_l, C_SPACE)
WCLASS(iswupper_l, C_UPPER) WCLASS(iswxdigit_l, C_XDIGIT)
static SYSV uint32_t s_towlower_l(uint32_t c, void *l) { (void)l; return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static SYSV uint32_t s_towupper_l(uint32_t c, void *l) { (void)l; return c >= 'a' && c <= 'z' ? c - 32 : c; }

// struct __locale_struct: 13 category pointers, then the ctype tables libc++ reads.
typedef struct { void *categories[13]; const uint16_t *ctype_b; const int32_t *lower, *upper; const char *names[13]; } LinuxLocale;
static LinuxLocale c_locale;
static SYSV LinuxLocale *s_newlocale(int mask, const char *name, LinuxLocale *base) {
    (void)mask; (void)base;
    if (name && *name && strcmp(name, "C") && strcmp(name, "POSIX") && strcmp(name, "C.UTF-8") && strcmp(name, "en_US.UTF-8")) {
        set_errno(L_ENOENT); return NULL;
    }
    return &c_locale;
}
static SYSV void s_freelocale(LinuxLocale *l) { (void)l; }
static SYSV LinuxLocale *s_uselocale(LinuxLocale *l) { (void)l; return (LinuxLocale *)-1; }
static SYSV char *s_setlocale(int category, const char *name) {
    (void)category;
    return !name || !*name || !strcmp(name, "C") || !strcmp(name, "POSIX") ? "C" : NULL;
}
// struct lconv on Linux: ten char pointers, then fourteen chars.
static struct { const char *text[10]; char values[14]; } c_lconv = {
    {".", "", "", "", "", "", "", "", "", ""}, {127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127}};
static SYSV void *s_localeconv(void) { return &c_lconv; }

static SYSV void *s_catopen(const char *name, int flag) { (void)name; (void)flag; return (void *)-1; }
static SYSV char *s_catgets(void *cat, int set, int msg, const char *text) { (void)cat; (void)set; (void)msg; return (char *)text; }
static SYSV int s_catclose(void *cat) { (void)cat; return 0; }

// ---- time ---------------------------------------------------------------------------

static int64_t filetime_ns(void) {
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    return (int64_t)((((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ull) * 100;
}
static int64_t monotonic_ns(void) {
    static LARGE_INTEGER frequency;
    LARGE_INTEGER now;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    return (int64_t)((double)now.QuadPart * 1e9 / (double)frequency.QuadPart);
}
static SYSV int s_clock_gettime(int clock, LinuxTimespec *ts) {
    int64_t ns;
    if (clock == 0 || clock == 5 || clock == 8) ns = filetime_ns();
    else if (clock == 2 || clock == 3) {
        FILETIME a, b, k, u;
        if (clock == 2) GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
        else GetThreadTimes(GetCurrentThread(), &a, &b, &k, &u);
        ns = (int64_t)((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100;
    } else ns = monotonic_ns();
    ts->sec = ns / 1000000000; ts->nsec = ns % 1000000000;
    return 0;
}
static SYSV int s_gettimeofday(int64_t *tv, void *tz) {
    (void)tz; int64_t ns = filetime_ns();
    tv[0] = ns / 1000000000; tv[1] = (ns % 1000000000) / 1000;
    return 0;
}
static SYSV int s_nanosleep(const LinuxTimespec *req, LinuxTimespec *rem) {
    if (rem) rem->sec = rem->nsec = 0;
    int64_t ms = req->sec * 1000 + (req->nsec + 999999) / 1000000;
    Sleep((DWORD)min(ms, (int64_t)0x7ffffffe));
    return 0;
}
static SYSV int s_poll(void *fds, uint64_t n, int timeout) {
    (void)fds;
    if (n) { set_errno(L_ENOSYS); return -1; }
    if (timeout > 0) Sleep((DWORD)timeout);
    return 0;
}

// struct tm on Linux has tm_gmtoff and tm_zone after the nine ints.
typedef struct { int sec, min, hour, mday, mon, year, wday, yday, isdst, pad; int64_t gmtoff; const char *zone; } LinuxTm;
static void to_linux_tm(LinuxTm *out, const struct tm *in, int64_t gmtoff) {
    out->sec = in->tm_sec; out->min = in->tm_min; out->hour = in->tm_hour; out->mday = in->tm_mday; out->mon = in->tm_mon;
    out->year = in->tm_year; out->wday = in->tm_wday; out->yday = in->tm_yday; out->isdst = in->tm_isdst;
    out->gmtoff = gmtoff; out->zone = gmtoff ? "LOC" : "GMT";
}
static void from_linux_tm(struct tm *out, const LinuxTm *in) {
    memset(out, 0, sizeof *out);
    out->tm_sec = in->sec; out->tm_min = in->min; out->tm_hour = in->hour; out->tm_mday = in->mday; out->tm_mon = in->mon;
    out->tm_year = in->year; out->tm_wday = in->wday; out->tm_yday = in->yday; out->tm_isdst = in->isdst;
}
static SYSV LinuxTm *s_gmtime_r(const int64_t *t, LinuxTm *out) {
    struct tm tm; __time64_t v = *t;
    if (_gmtime64_s(&tm, &v)) { set_errno(L_EINVAL); return NULL; }
    to_linux_tm(out, &tm, 0); return out;
}
static SYSV LinuxTm *s_localtime_r(const int64_t *t, LinuxTm *out) {
    struct tm tm; __time64_t v = *t;
    if (_localtime64_s(&tm, &v)) { set_errno(L_EINVAL); return NULL; }
    long offset = 0; _get_timezone(&offset);
    to_linux_tm(out, &tm, -(int64_t)offset); return out;
}
static SYSV int64_t s_mktime(LinuxTm *in) {
    struct tm tm; from_linux_tm(&tm, in);
    int64_t r = _mktime64(&tm);
    long offset = 0; _get_timezone(&offset);
    to_linux_tm(in, &tm, -(int64_t)offset);
    return r;
}
static SYSV size_t s_strftime_l(char *s, size_t max, const char *fmt, const LinuxTm *in, void *l) {
    (void)l; struct tm tm; from_linux_tm(&tm, in);
    return strftime(s, max, fmt, &tm);
}
static char *tzname_values[2] = {"GMT", "GMT"};
static int64_t timezone_value;
static SYSV void s_tzset(void) {}

// ---- process and system ------------------------------------------------------------

static char *environ_values[] = {NULL};
static char **guest_environ = environ_values;
static SYSV char *s_getenv(const char *name) { return getenv(name); }
static SYSV int s_getpid(void) { return (int)GetCurrentProcessId(); }
static SYSV unsigned s_geteuid(void) { return 1000; }
static SYSV int s_getpagesize(void) { return 4096; }
static SYSV int64_t s_sysconf(int name) {
    SYSTEM_INFO info; GetSystemInfo(&info);
    switch (name) {
    case 30: return 4096;                                  // _SC_PAGESIZE
    case 83: case 84: return info.dwNumberOfProcessors;    // _SC_NPROCESSORS_CONF / ONLN
    case 2: return 100;                                    // _SC_CLK_TCK
    case 4: return 1024;                                   // _SC_OPEN_MAX
    case 85: {                                             // _SC_PHYS_PAGES
        MEMORYSTATUSEX m = {sizeof m}; GlobalMemoryStatusEx(&m); return (int64_t)(m.ullTotalPhys / 4096);
    }
    case 0x46: return 64;                                  // _SC_LEVEL1_DCACHE_LINESIZE
    default: set_errno(L_EINVAL); return -1;
    }
}
static uint8_t auxv_random[16];
static SYSV uint64_t s_getauxval(uint64_t type) {
    switch (type) {
    case 6: return 4096;                      // AT_PAGESZ
    case 25: return (uintptr_t)auxv_random;   // AT_RANDOM
    default: set_errno(L_ENOENT); return 0;
    }
}
typedef struct { uint64_t cur, max; } RLimit;
static SYSV int s_getrlimit(int resource, RLimit *r) {
    r->cur = r->max = resource == 3 ? 8u << 20 : resource == 7 ? 1024 : UINT64_MAX;
    return 0;
}
static SYSV int s_getentropy(void *buf, size_t n) {
    if (n > 256) { set_errno(5); return -1; }
    BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return 0;
}
static SYSV int64_t s_syscall(int64_t number, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e) {
    (void)e;
    switch (number) {
    case 186: return GetCurrentThreadId();                          // gettid
    case 39: return GetCurrentProcessId();                          // getpid
    case 9: {                                                       // mmap
        void *p = s_mmap((void *)a, (size_t)b, (int)c, (int)d, (int)e, 0);
        return p == (void *)-1 ? -1 : (int64_t)(uintptr_t)p;
    }
    case 10: return s_mprotect((void *)a, (size_t)b, (int)c);
    case 11: return s_munmap((void *)a, (size_t)b);
    case 28: return 0;                                              // madvise
    case 1:                                                         // write; absl logs this way
        if (a == 1 || a == 2) { fwrite((const void *)b, 1, (size_t)c, stderr); fflush(stderr); return c; }
        return s_write((int)a, (const void *)b, (size_t)c);
    case 318: BCryptGenRandom(NULL, (void *)a, (ULONG)b, BCRYPT_USE_SYSTEM_PREFERRED_RNG); return b; // getrandom
    case 204: {                                                     // sched_getaffinity
        DWORD_PTR process, system;
        if (b < 8 || !GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) { set_errno(L_EINVAL); return -1; }
        memset((void *)c, 0, (size_t)b);
        memcpy((void *)c, &process, 8);
        return 8;
    }
    case 202: {                                                     // futex
        int op = (int)b & 0x7f;
        if (op == 0 || op == 9) {
            DWORD millis = INFINITE;
            if (d) {
                const LinuxTimespec *t = (const LinuxTimespec *)d;
                millis = op == 9 ? wait_millis(t) : (DWORD)(t->sec * 1000 + t->nsec / 1000000);
            }
            int32_t expected = (int32_t)c;
            if (*(volatile int32_t *)a != expected) { set_errno(L_EAGAIN); return -1; }
            if (!WaitOnAddress((void *)a, &expected, 4, millis)) { set_errno(L_ETIMEDOUT); return -1; }
            return 0;
        }
        if (op == 1 || op == 10) {
            if (c >= INT_MAX) WakeByAddressAll((void *)a); else for (int64_t i = 0; i < c; ++i) WakeByAddressSingle((void *)a);
            return 0;
        }
        break;
    }
    }
    set_errno(L_ENOSYS);
    return -1;
}
static SYSV void s_exit(int code) { fflush(NULL); ExitProcess((UINT)code); }
static void describe_caller(void *address) {
    for (unsigned i = 0; i < elf_object_count; ++i) {
        ElfObject *o = &elf_objects[i];
        if ((uint8_t *)address >= o->base && (uint8_t *)address < o->base + o->size)
            fprintf(stderr, " (called from %s+%#llx)", o->name, (unsigned long long)((uint8_t *)address - o->base));
    }
}
__attribute__((noinline)) static SYSV void s_abort(void) {
    fprintf(stderr, "The engine aborted");
    if (getenv("GTTS_TRACE")) describe_caller(__builtin_return_address(0));
    fprintf(stderr, ".\n"); fflush(stderr); ExitProcess(134);
}
static SYSV void s_stack_chk_fail(void) { fprintf(stderr, "Stack corruption detected in the engine.\n"); fflush(stderr); ExitProcess(134); }
static SYSV void s_assert_fail(const char *expr, const char *file, unsigned line, const char *fn) {
    fprintf(stderr, "Engine assertion failed: %s (%s:%u %s)\n", expr, file, line, fn ? fn : "");
    fflush(stderr); ExitProcess(134);
}

typedef struct { void *fname, *fbase, *sname, *saddr; } DlInfo;
static SYSV int s_dladdr(const void *addr, DlInfo *info) {
    for (unsigned i = 0; i < elf_object_count; ++i) {
        ElfObject *o = &elf_objects[i];
        if ((uint8_t *)addr >= o->base && (uint8_t *)addr < o->base + o->size) {
            info->fname = o->name; info->fbase = o->base; info->sname = NULL; info->saddr = NULL; return 1;
        }
    }
    return 0;
}
static SYSV void *s_dlopen(const char *name, int flags) { (void)name; (void)flags; return NULL; }
static SYSV void *s_dlsym(void *handle, const char *name) { (void)handle; return elf_symbol(name); }

typedef struct { uint64_t addr; const char *name; const ElfPhdr *phdr; uint16_t phnum; uint64_t adds, subs, tls_modid; void *tls_data; } PhdrInfo;
typedef SYSV int (*PhdrCallback)(PhdrInfo *, size_t, void *);
static SYSV int s_dl_iterate_phdr(PhdrCallback callback, void *data) {
    for (unsigned i = 0; i < elf_object_count; ++i) {
        ElfObject *o = &elf_objects[i];
        PhdrInfo info = {(uintptr_t)o->base, o->name, o->phdr, (uint16_t)o->phnum, elf_object_count, 0, o->tls_module, NULL};
        int r = callback(&info, sizeof info, data);
        if (r) return r;
    }
    return 0;
}
typedef struct { uint64_t flags; void *map_start, *map_end, *link_map, *eh_frame; uint64_t reserved[7]; } FindObject;
static SYSV int s_dl_find_object(void *pc, FindObject *out) {
    for (unsigned i = 0; i < elf_object_count; ++i) {
        ElfObject *o = &elf_objects[i];
        if ((uint8_t *)pc >= o->base && (uint8_t *)pc < o->base + o->size) {
            memset(out, 0, sizeof *out);
            out->map_start = o->base; out->map_end = o->base + o->size; out->eh_frame = o->base + o->eh_frame;
            return 0;
        }
    }
    return -1;
}
static SYSV void s_dl_get_tls_static_info(size_t *size, size_t *align) { *size = 0; *align = 64; }

// glibc's rand(): the TYPE_3 additive feedback generator.
static SRWLOCK random_lock = SRWLOCK_INIT;
static int32_t random_state[34];
static int random_index = -1;
static void glibc_srand(uint32_t seed) {
    random_state[0] = (int32_t)(seed ? seed : 1);
    for (int i = 1; i < 31; ++i) {
        int64_t hi = random_state[i - 1] / 127773, lo = random_state[i - 1] % 127773;
        int64_t word = 16807 * lo - 2836 * hi;
        if (word < 0) word += 2147483647;
        random_state[i] = (int32_t)word;
    }
    for (int i = 31; i < 34; ++i) random_state[i] = random_state[i - 31];
    random_index = 34;
    // Discard 310 outputs, as glibc does after seeding.
    for (int i = 0; i < 310; ++i) {
        int k = random_index % 34;
        random_state[k] = random_state[(random_index - 31) % 34] + random_state[(random_index - 3) % 34];
        ++random_index;
    }
}
static SYSV void s_srand(unsigned seed) { AcquireSRWLockExclusive(&random_lock); glibc_srand(seed); ReleaseSRWLockExclusive(&random_lock); }
static SYSV int s_rand(void) {
    AcquireSRWLockExclusive(&random_lock);
    if (random_index < 0) glibc_srand(1);
    int k = random_index % 34;
    random_state[k] = random_state[(random_index - 31) % 34] + random_state[(random_index - 3) % 34];
    int r = (int)((uint32_t)random_state[k] >> 1);
    ++random_index;
    if (random_index > 1000000000) random_index = 34 + random_index % 34;
    ReleaseSRWLockExclusive(&random_lock);
    return r;
}

// qsort has no context argument; the comparator travels in the calling thread's state.
static int sort_trampoline(const void *a, const void *b) { return current()->sort_compare(a, b); }
static SYSV void s_qsort(void *base, size_t n, size_t size, Compare compare) {
    GuestThread *t = current();
    Compare previous = t->sort_compare;
    t->sort_compare = compare;
    qsort(base, n, size, sort_trampoline);
    t->sort_compare = previous;
}
static SYSV uint64_t s_div(int a, int b) { div_t d = div(a, b); return (uint32_t)d.quot | ((uint64_t)(uint32_t)d.rem << 32); }

// ---- math -----------------------------------------------------------------------------

#define M1(n) static SYSV double s_##n(double x) { return n(x); }
#define M1F(n) static SYSV float s_##n(float x) { return n(x); }
#define M2(n) static SYSV double s_##n(double x, double y) { return n(x, y); }
#define M2F(n) static SYSV float s_##n(float x, float y) { return n(x, y); }
M1(asin) M1(atan) M1(cos) M1(sin) M1(tan) M1(exp) M1(log) M1(log10) M1(log1p) M1(log2) M1(sqrt) M1(ceil) M1(floor)
M1(rint) M1(round) M1(erf) M2(atan2) M2(fmod) M2(pow) M2(hypot)
M1F(cbrtf) M1F(ceilf) M1F(floorf) M1F(cosf) M1F(sinf) M1F(expf) M1F(expm1f) M1F(erfcf) M1F(logf) M1F(log10f) M1F(log2f)
M1F(sqrtf) M1F(rintf) M1F(roundf) M1F(tanhf) M1F(nearbyintf) M2F(atan2f) M2F(fmodf) M2F(powf) M2F(hypotf) M2F(nextafterf)
static SYSV double s_ldexp(double x, int e) { return ldexp(x, e); }
static SYSV double s_frexp(double x, int *e) { return frexp(x, e); }
static SYSV double s_modf(double x, double *i) { return modf(x, i); }
static SYSV int64_t s_lrintf(float x) { return llrintf(x); }
static SYSV double s_nan(const char *s) { (void)s; return NAN; }
static SYSV float s_nanf(const char *s) { (void)s; return NAN; }
static SYSV void s_sincos(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }
static SYSV void s_sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }

// ---- engine logger ------------------------------------------------------------------

int engine_verbose;
Speaker engine_speakers[128];
unsigned engine_speaker_count;

// ---- tables ---------------------------------------------------------------------------

static SYSV int s_return_zero(void) { return 0; }
static SYSV int s_return_minus_one(void) { set_errno(L_ENOSYS); return -1; }
static SYSV void s_noop(void) {}

typedef struct { const char *name; void *fn; } Shim;
#define S(name, fn) {name, (void *)(fn)}
static const Shim shims[] = {
    S("__errno_location", s_errno_location), S("__tls_get_addr", s_tls_get_addr),
    S("pthread_create", s_pthread_create), S("pthread_join", s_pthread_join), S("pthread_detach", s_pthread_detach),
    S("pthread_self", s_pthread_self), S("pthread_key_create", s_pthread_key_create),
    S("pthread_getspecific", s_pthread_getspecific), S("pthread_setspecific", s_pthread_setspecific),
    S("pthread_once", s_pthread_once), S("pthread_attr_init", s_pthread_attr_init), S("pthread_attr_destroy", s_pthread_attr_destroy),
    S("pthread_attr_setstacksize", s_pthread_attr_setstacksize), S("pthread_attr_setdetachstate", s_pthread_attr_setdetachstate),
    S("pthread_attr_setguardsize", s_zero_int), S("pthread_attr_setinheritsched", s_zero_int),
    S("pthread_attr_setschedparam", s_zero_int), S("pthread_attr_setschedpolicy", s_zero_int),
    S("pthread_attr_setscope", s_zero_int), S("pthread_attr_getschedparam", s_zero_int),
    S("pthread_getschedparam", s_zero_int), S("pthread_setname_np", s_zero_int), S("pthread_kill", s_zero_int),
    S("pthread_sigmask", s_zero_int), S("pthread_mutex_init", s_pthread_mutex_init), S("pthread_mutex_destroy", s_zero_int),
    S("pthread_mutex_lock", s_pthread_mutex_lock), S("pthread_mutex_trylock", s_pthread_mutex_trylock),
    S("pthread_mutex_unlock", s_pthread_mutex_unlock), S("pthread_mutexattr_init", s_pthread_mutexattr_init),
    S("pthread_mutexattr_destroy", s_zero_int), S("pthread_mutexattr_settype", s_pthread_mutexattr_settype),
    S("pthread_cond_init", s_pthread_cond_init), S("pthread_cond_destroy", s_zero_int),
    S("pthread_cond_signal", s_pthread_cond_signal), S("pthread_cond_broadcast", s_pthread_cond_broadcast),
    S("pthread_cond_wait", s_pthread_cond_wait), S("pthread_cond_timedwait", s_pthread_cond_timedwait),
    S("pthread_rwlock_rdlock", s_pthread_rwlock_rdlock), S("pthread_rwlock_wrlock", s_pthread_rwlock_wrlock),
    S("pthread_rwlock_unlock", s_pthread_rwlock_unlock),
    S("__pthread_register_cancel", s_noop), S("__pthread_unregister_cancel", s_noop), S("__pthread_unwind_next", s_abort),
    S("sem_init", s_sem_init), S("sem_destroy", s_zero_int), S("sem_post", s_sem_post), S("sem_wait", s_sem_wait),
    S("sem_timedwait", s_sem_timedwait), S("sched_yield", s_sched_yield), S("sched_getcpu", s_sched_getcpu),
    S("sched_get_priority_max", s_zero_int), S("nice", s_zero_int), S("__cxa_thread_atexit_impl", s_cxa_thread_atexit_impl),
    S("__cxa_atexit", s_zero_int), S("__cxa_finalize", s_noop), S("__sigsetjmp", s_zero_int),
    S("malloc", s_malloc), S("free", s_free), S("calloc", s_calloc), S("realloc", s_realloc),
    S("posix_memalign", s_posix_memalign), S("aligned_alloc", s_aligned_alloc), S("memalign", s_memalign),
    S("strdup", s_strdup), S("mmap", s_mmap), S("mmap64", s_mmap), S("munmap", s_munmap), S("mprotect", s_mprotect),
    S("madvise", s_zero_int), S("mlock", s_zero_int), S("munlock", s_zero_int), S("__sbrk", s_sbrk),
    S("open", s_open), S("open64", s_open), S("__open64_2", s_open_2), S("openat", s_openat), S("close", s_close),
    S("read", s_read), S("__read_chk", s_read_chk), S("write", s_write), S("lseek64", s_lseek), S("lseek", s_lseek),
    S("pread64", s_pread), S("__pread64_chk", s_pread_chk), S("dup", s_dup), S("ftruncate64", s_ftruncate),
    S("ftruncate", s_ftruncate), S("fallocate64", s_fallocate), S("isatty", s_isatty), S("ioctl", s_return_minus_one),
    S("stat64", s_stat), S("stat", s_stat), S("lstat", s_stat), S("fstat64", s_fstat), S("fstat", s_fstat),
    S("access", s_access), S("mkdir", s_mkdir), S("remove", s_remove), S("unlinkat", s_unlinkat), S("rename", s_rename),
    S("getcwd", s_getcwd), S("realpath", s_realpath), S("__realpath_chk", s_realpath_chk), S("readlink", s_readlink),
    S("__readlink_chk", s_readlink_chk), S("opendir", s_opendir), S("readdir64", s_readdir), S("readdir", s_readdir),
    S("closedir", s_closedir), S("chdir", s_return_minus_one), S("fchmod", s_zero_int), S("fchmodat", s_zero_int),
    S("utimensat", s_zero_int), S("link", s_return_minus_one), S("symlink", s_return_minus_one),
    S("fopen", s_fopen), S("fopen64", s_fopen), S("fdopen", s_fdopen), S("fclose", s_fclose), S("fread", s_fread),
    S("__fread_chk", s_fread_chk), S("fwrite", s_fwrite), S("fflush", s_fflush), S("feof", s_feof), S("fgetc", s_fgetc),
    S("getc", s_fgetc), S("ungetc", s_ungetc), S("fgets", s_fgets), S("__fgets_chk", s_fgets_chk), S("fputc", s_fputc),
    S("fseek", s_fseek), S("fseeko64", s_fseek), S("fseeko", s_fseek), S("ftell", s_ftell), S("ftello64", s_ftell),
    S("ftello", s_ftell), S("rewind", s_rewind), S("fileno", s_fileno),
    S("fprintf", s_fprintf), S("__fprintf_chk", s_fprintf_chk), S("__vfprintf_chk", s_vfprintf_chk),
    S("__vprintf_chk", s_vprintf_chk), S("snprintf", s_snprintf), S("__vsnprintf_chk", s_vsnprintf_chk),
    S("__vsprintf_chk", s_vsprintf_chk), S("__vasprintf_chk", s_vasprintf_chk), S("swprintf", s_swprintf),
    S("__isoc99_sscanf", s_sscanf), S("__isoc23_sscanf", s_sscanf), S("sscanf", s_sscanf), S("__isoc23_vsscanf", s_vsscanf),
    S("memcpy", s_memcpy), S("memmove", s_memmove), S("memset", s_memset), S("__memcpy_chk", s_memcpy_chk),
    S("__memmove_chk", s_memmove_chk), S("__memset_chk", s_memset_chk), S("memcmp", s_memcmp), S("bcmp", s_memcmp),
    S("memchr", s_memchr), S("strlen", s_strlen), S("strnlen", s_strnlen), S("strcmp", s_strcmp), S("strncmp", s_strncmp),
    S("strcoll_l", s_strcoll_l), S("strxfrm_l", s_strxfrm_l), S("strcpy", s_strcpy), S("strncpy", s_strncpy),
    S("strcat", s_strcat), S("strncat", s_strncat), S("__strcpy_chk", s_strcpy_chk), S("__strncpy_chk", s_strncpy_chk),
    S("__strcat_chk", s_strcat_chk), S("__strncat_chk", s_strncat_chk), S("strchr", s_strchr), S("strrchr", s_strrchr),
    S("strstr", s_strstr), S("strerror", s_strerror), S("strerror_r", s_strerror_r),
    S("strtol", s_strtol), S("strtoll", s_strtol), S("strtoul", s_strtoul), S("strtoull", s_strtoul),
    S("__isoc23_strtol", s_strtol), S("__isoc23_strtoll", s_strtol), S("__isoc23_strtoul", s_strtoul),
    S("__isoc23_strtoull", s_strtoul), S("strtoll_l", s_strtoll_l), S("strtoull_l", s_strtoull_l),
    S("__isoc23_strtoll_l", s_strtoll_l), S("__isoc23_strtoull_l", s_strtoull_l), S("strtod", s_strtod),
    S("strtof", s_strtof), S("strtold", s_strtold), S("strtod_l", s_strtod_l), S("strtof_l", s_strtof_l),
    S("strtold_l", s_strtold_l), S("wcslen", s_wcslen), S("wmemchr", s_wmemchr), S("wmemcmp", s_wmemcmp),
    S("btowc", s_btowc), S("wctob", s_wctob), S("mbrtowc", s_mbrtowc), S("mbtowc", s_mbtowc), S("wcrtomb", s_wcrtomb),
    S("mbsnrtowcs", s_mbsnrtowcs), S("mbsrtowcs", s_mbsrtowcs), S("wcsnrtombs", s_wcsnrtombs),
    S("__ctype_get_mb_cur_max", s_mb_cur_max), S("__ctype_b_loc", s_ctype_b_loc), S("__ctype_tolower_loc", s_ctype_tolower_loc),
    S("__ctype_toupper_loc", s_ctype_toupper_loc), S("tolower_l", s_tolower_l), S("toupper_l", s_toupper_l),
    S("iswalpha_l", s_iswalpha_l), S("iswblank_l", s_iswblank_l), S("iswcntrl_l", s_iswcntrl_l), S("iswdigit_l", s_iswdigit_l),
    S("iswlower_l", s_iswlower_l), S("iswprint_l", s_iswprint_l), S("iswpunct_l", s_iswpunct_l), S("iswspace_l", s_iswspace_l),
    S("iswupper_l", s_iswupper_l), S("iswxdigit_l", s_iswxdigit_l), S("towlower_l", s_towlower_l), S("towupper_l", s_towupper_l),
    S("newlocale", s_newlocale), S("freelocale", s_freelocale), S("uselocale", s_uselocale), S("setlocale", s_setlocale),
    S("localeconv", s_localeconv), S("catopen", s_catopen), S("catgets", s_catgets), S("catclose", s_catclose),
    S("clock_gettime", s_clock_gettime), S("gettimeofday", s_gettimeofday), S("nanosleep", s_nanosleep), S("poll", s_poll),
    S("gmtime_r", s_gmtime_r), S("localtime_r", s_localtime_r), S("mktime", s_mktime), S("strftime_l", s_strftime_l),
    S("tzset", s_tzset), S("getenv", s_getenv), S("getpid", s_getpid), S("geteuid", s_geteuid), S("getpagesize", s_getpagesize),
    S("sysconf", s_sysconf), S("__sysconf", s_sysconf), S("getauxval", s_getauxval), S("getrlimit64", s_getrlimit),
    S("setrlimit64", s_zero_int), S("getentropy", s_getentropy), S("syscall", s_syscall), S("prctl", s_zero_int),
    S("kill", s_zero_int), S("sigaction", s_zero_int), S("sigaltstack", s_zero_int), S("sigdelset", s_zero_int),
    S("sigfillset", s_zero_int), S("exit", s_exit), S("_exit", s_exit), S("abort", s_abort),
    S("__stack_chk_fail", s_stack_chk_fail), S("__assert_fail", s_assert_fail), S("dladdr", s_dladdr), S("dlopen", s_dlopen),
    S("dlsym", s_dlsym), S("dl_iterate_phdr", s_dl_iterate_phdr), S("_dl_find_object", s_dl_find_object),
    S("_dl_get_tls_static_info", s_dl_get_tls_static_info), S("srand", s_srand), S("rand", s_rand), S("qsort", s_qsort),
    S("div", s_div), S("asin", s_asin), S("atan", s_atan), S("cos", s_cos), S("sin", s_sin), S("tan", s_tan), S("exp", s_exp),
    S("log", s_log), S("log10", s_log10), S("log1p", s_log1p), S("log2", s_log2), S("sqrt", s_sqrt), S("ceil", s_ceil),
    S("floor", s_floor), S("rint", s_rint), S("round", s_round), S("erf", s_erf), S("atan2", s_atan2), S("fmod", s_fmod),
    S("pow", s_pow), S("hypot", s_hypot), S("cbrtf", s_cbrtf), S("ceilf", s_ceilf), S("floorf", s_floorf), S("cosf", s_cosf),
    S("sinf", s_sinf), S("expf", s_expf), S("expm1f", s_expm1f), S("erfcf", s_erfcf), S("logf", s_logf), S("log10f", s_log10f),
    S("log2f", s_log2f), S("sqrtf", s_sqrtf), S("rintf", s_rintf), S("roundf", s_roundf), S("tanhf", s_tanhf),
    S("nearbyintf", s_nearbyintf), S("atan2f", s_atan2f), S("fmodf", s_fmodf), S("powf", s_powf), S("hypotf", s_hypotf),
    S("nextafterf", s_nextafterf), S("ldexp", s_ldexp), S("frexp", s_frexp), S("modf", s_modf), S("lrintf", s_lrintf),
    S("nan", s_nan), S("nanf", s_nanf), S("sincos", s_sincos), S("sincosf", s_sincosf),
    S("__register_frame_info", s_noop), S("__deregister_frame_info", s_return_zero),
};

void *shim_lookup(const char *name) {
    for (size_t i = 0; i < sizeof shims / sizeof shims[0]; ++i) if (!strcmp(shims[i].name, name)) return shims[i].fn;
    return NULL;
}

void *shim_data_lookup(const char *name) {
    if (!strcmp(name, "stdin")) return &guest_stdin_slot;
    if (!strcmp(name, "stdout")) return &guest_stdout_slot;
    if (!strcmp(name, "stderr")) return &guest_stderr_slot;
    if (!strcmp(name, "environ") || !strcmp(name, "__environ")) return &guest_environ;
    if (!strcmp(name, "tzname")) return tzname_values;
    if (!strcmp(name, "__timezone") || !strcmp(name, "timezone")) return &timezone_value;
    return NULL;
}

void shim_init(void) {
    if (thread_slot != TLS_OUT_OF_INDEXES) return;
    thread_slot = TlsAlloc();
    init_ctype();
    c_locale.ctype_b = ctype_pointer; c_locale.lower = lower_pointer; c_locale.upper = upper_pointer;
    for (int i = 0; i < 13; ++i) c_locale.names[i] = "C";
    BCryptGenRandom(NULL, auxv_random, sizeof auxv_random, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    // Anything the engine prints would corrupt the worker protocol on stdout.
    FILE *quiet = fopen("NUL", "wb");
    guest_stdin = fopen("NUL", "rb");
    guest_stdout = engine_verbose ? stderr : quiet;
    guest_stderr = engine_verbose ? stderr : quiet;
    guest_stdin_slot = guest_stdin; guest_stdout_slot = guest_stdout; guest_stderr_slot = guest_stderr;
}
