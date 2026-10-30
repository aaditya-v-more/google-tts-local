#include "guest.h"
#include "quad.h"

#include <dlfcn.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <ffi/ffi.h>
#include <limits.h>
#include <langinfo.h>
#include <locale.h>
#include <math.h>
#include <xlocale.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

typedef uint64_t (*GenericFunction)(uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
static void *guest_malloc(Guest *g, size_t size);
static void *guest_aligned_malloc(Guest *g,size_t alignment,size_t size);
static void guest_free(Guest *g, void *pointer);

// Linux arm64 uses asm-generic errno values; Darwin uses a different sequence.
// https://github.com/torvalds/linux/blob/v6.8/include/uapi/asm-generic/errno.h
static const struct { int host, linux; } errno_map[] = {
    {EAGAIN,11}, {EDEADLK,35}, {ENAMETOOLONG,36}, {ENOLCK,37}, {ENOSYS,38},
    {ENOTEMPTY,39}, {ELOOP,40}, {ENOMSG,42}, {EIDRM,43}, {ENOSTR,60},
    {ENODATA,61}, {ETIME,62}, {ENOSR,63}, {EREMOTE,66}, {ENOLINK,67},
    {EPROTO,71}, {EMULTIHOP,72}, {EBADMSG,74}, {EOVERFLOW,75}, {EILSEQ,84},
    {EUSERS,87}, {ENOTSOCK,88}, {EDESTADDRREQ,89}, {EMSGSIZE,90}, {EPROTOTYPE,91},
    {ENOPROTOOPT,92}, {EPROTONOSUPPORT,93}, {ESOCKTNOSUPPORT,94}, {ENOTSUP,95},
    {EPFNOSUPPORT,96}, {EAFNOSUPPORT,97}, {EADDRINUSE,98}, {EADDRNOTAVAIL,99},
    {ENETDOWN,100}, {ENETUNREACH,101}, {ENETRESET,102}, {ECONNABORTED,103},
    {ECONNRESET,104}, {ENOBUFS,105}, {EISCONN,106}, {ENOTCONN,107},
    {ESHUTDOWN,108}, {ETOOMANYREFS,109}, {ETIMEDOUT,110}, {ECONNREFUSED,111},
    {EHOSTDOWN,112}, {EHOSTUNREACH,113}, {EALREADY,114}, {EINPROGRESS,115},
    {ESTALE,116}, {EDQUOT,122}, {ECANCELED,125}, {EOWNERDEAD,130}, {ENOTRECOVERABLE,131}
};
static int linux_errno(int value) {
    if(value>=0 && value<=34 && value!=EDEADLK)return value;
    for(size_t i=0;i<sizeof errno_map/sizeof errno_map[0];++i)if(errno_map[i].host==value)return errno_map[i].linux;
    return 5; // No Linux equivalent: EIO.
}
static int host_errno(int value) {
    if(value>=0 && value<=34 && value!=11)return value;
    for(size_t i=0;i<sizeof errno_map/sizeof errno_map[0];++i)if(errno_map[i].linux==value)return errno_map[i].host;
    return EIO;
}

// glibc rand() uses the default 31-word additive-feedback random() state.
static pthread_mutex_t random_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t random_state[31];
static unsigned random_front, random_rear;
static int random_initialized;

static uint32_t linux_rand_next(void) {
    uint32_t sum = random_state[random_front] + random_state[random_rear];
    random_state[random_front] = sum;
    random_front = (random_front + 1) % 31;
    random_rear = (random_rear + 1) % 31;
    return sum >> 1;
}

static void linux_srand(uint32_t seed) {
    if (!seed) seed = 1;
    random_state[0] = seed;
    int64_t word = (int32_t)seed;
    for (unsigned i = 1; i < 31; ++i) {
        int64_t high = word / 127773, low = word % 127773;
        word = 16807 * low - 2836 * high;
        if (word < 0) word += 2147483647;
        random_state[i] = (uint32_t)word;
    }
    random_front = 3; random_rear = 0;
    for (unsigned i = 0; i < 310; ++i) (void)linux_rand_next();
    random_initialized = 1;
}

static pthread_mutex_t *guest_mutex(uint64_t address, int kind) {
    if (!address) return NULL;
    uintptr_t *slot = (uintptr_t *)(uintptr_t)address;
    uintptr_t existing = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (existing) return (pthread_mutex_t *)existing;
    pthread_mutex_t *mutex = malloc(sizeof(*mutex));
    if (!mutex) return NULL;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    if (kind == 1) pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (kind == 2) pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    int result = pthread_mutex_init(mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    if (result) { free(mutex); return NULL; }
    uintptr_t expected = 0;
    if (!__atomic_compare_exchange_n(slot, &expected, (uintptr_t)mutex, 0,
                                     __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
        pthread_mutex_destroy(mutex);
        free(mutex);
        return (pthread_mutex_t *)expected;
    }
    return mutex;
}

static pthread_cond_t *guest_cond(uint64_t address) {
    if (!address) return NULL;
    uintptr_t *slot = (uintptr_t *)(uintptr_t)address;
    uintptr_t existing = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (existing) return (pthread_cond_t *)existing;
    pthread_cond_t *condition = malloc(sizeof(*condition));
    if (!condition || pthread_cond_init(condition, NULL)) { free(condition); return NULL; }
    uintptr_t expected = 0;
    if (!__atomic_compare_exchange_n(slot, &expected, (uintptr_t)condition, 0,
                                     __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
        pthread_cond_destroy(condition);
        free(condition);
        return (pthread_cond_t *)expected;
    }
    return condition;
}

static pthread_rwlock_t *guest_rwlock(uint64_t address) {
    if(!address)return NULL;uintptr_t *slot=(uintptr_t *)(uintptr_t)address;
    uintptr_t existing=__atomic_load_n(slot,__ATOMIC_ACQUIRE);if(existing)return (pthread_rwlock_t *)existing;
    pthread_rwlock_t *lock=malloc(sizeof *lock);if(!lock || pthread_rwlock_init(lock,NULL)){free(lock);return NULL;}
    uintptr_t expected=0;
    if(!__atomic_compare_exchange_n(slot,&expected,(uintptr_t)lock,0,__ATOMIC_RELEASE,__ATOMIC_ACQUIRE)) {
        pthread_rwlock_destroy(lock);free(lock);return (pthread_rwlock_t *)expected;
    }
    return lock;
}
typedef struct {pthread_mutex_t lock;pthread_cond_t cond;unsigned value,waiters;} GuestSemaphore;
static uint64_t guest_semaphore(Guest *g,const char *name,const uint64_t a[8]) {
    uintptr_t *slot=(uintptr_t *)(uintptr_t)a[0];if(!slot){*(int *)(uintptr_t)g->errno_slot=22;return UINT64_MAX;}
    if(!strcmp(name,"sem_init")) {
        if(a[1] || a[2]>INT_MAX){*(int *)(uintptr_t)g->errno_slot=a[1]?38:22;return UINT64_MAX;}
        GuestSemaphore *sem=calloc(1,sizeof *sem);if(!sem){*(int *)(uintptr_t)g->errno_slot=12;return UINT64_MAX;}
        int error=pthread_mutex_init(&sem->lock,NULL);
        if(!error){error=pthread_cond_init(&sem->cond,NULL);if(error)pthread_mutex_destroy(&sem->lock);}
        if(error){free(sem);*(int *)(uintptr_t)g->errno_slot=linux_errno(error);return UINT64_MAX;}
        sem->value=(unsigned)a[2];__atomic_store_n(slot,(uintptr_t)sem,__ATOMIC_RELEASE);return 0;
    }
    GuestSemaphore *sem=(GuestSemaphore *)__atomic_load_n(slot,__ATOMIC_ACQUIRE);
    if(!sem){*(int *)(uintptr_t)g->errno_slot=22;return UINT64_MAX;}
    pthread_mutex_lock(&sem->lock);int error=0;
    if(!strcmp(name,"sem_post")) {
        if(sem->value==INT_MAX)error=EOVERFLOW;else {++sem->value;pthread_cond_signal(&sem->cond);}
    } else if(!strcmp(name,"sem_destroy")) {
        if(sem->waiters)error=EBUSY;else {pthread_mutex_unlock(&sem->lock);pthread_cond_destroy(&sem->cond);pthread_mutex_destroy(&sem->lock);
            __atomic_store_n(slot,0,__ATOMIC_RELEASE);free(sem);return 0;}
    } else if(!strcmp(name,"sem_getvalue")) {
        *(int *)(uintptr_t)a[1]=(int)sem->value;
    } else if(!strcmp(name,"sem_trywait") && !sem->value)error=EAGAIN;
    else {
        ++sem->waiters;
        while(!sem->value && !error) {
            if(!strcmp(name,"sem_timedwait")) {
                const struct timespec *deadline=(const struct timespec *)(uintptr_t)a[1];
                if(!deadline || deadline->tv_nsec<0 || deadline->tv_nsec>=1000000000)error=EINVAL;
                else error=pthread_cond_timedwait(&sem->cond,&sem->lock,deadline);
            } else error=pthread_cond_wait(&sem->cond,&sem->lock);
        }
        --sem->waiters;if(!error)--sem->value;
    }
    pthread_mutex_unlock(&sem->lock);
    if(error){*(int *)(uintptr_t)g->errno_slot=linux_errno(error);return UINT64_MAX;}return 0;
}

// glibc exposes its locale struct to libc++, including ctype tables at
// offsets 0x68, 0x70 and 0x78. Darwin's locale_t has a different layout.
static uint64_t guest_newlocale(Guest *g, const char *name) {
    locale_t host=newlocale(LC_ALL_MASK,name ? name : "C",NULL);
    if(!host){*(int *)(uintptr_t)g->errno_slot=linux_errno(errno);return 0;}
    uint8_t *locale = guest_malloc(g, 256);
    unsigned short *classes = guest_malloc(g, 384 * sizeof(*classes));
    int *lower = guest_malloc(g, 384 * sizeof(*lower));
    int *upper = guest_malloc(g, 384 * sizeof(*upper));
    const char *locale_name=name ? name : "C";size_t name_len=strlen(locale_name)+1;
    char *guest_name=guest_malloc(g,name_len);
    if (!locale || !classes || !lower || !upper || !guest_name) {
        guest_free(g,locale);guest_free(g,classes);guest_free(g,lower);guest_free(g,upper);guest_free(g,guest_name);freelocale(host);
        *(int *)(uintptr_t)g->errno_slot=12;return 0;
    }
    memset(locale,0,256);memcpy(guest_name,locale_name,name_len);
    const char *codeset=nl_langinfo_l(CODESET,host);
    int byte_only=codeset && (!strcasecmp(codeset,"UTF-8") || !strcasecmp(codeset,"US-ASCII") || !strcasecmp(codeset,"ASCII"));
    for (int i = -128; i < 256; ++i) {
        int c = i < -1 ? i + 256 : i;
        unsigned bits = 0;
        if (c >= 0 && !(byte_only && c>=128)) {
            if (isupper_l(c,host)) bits |= 0x0100;
            if (islower_l(c,host)) bits |= 0x0200;
            if (isalpha_l(c,host)) bits |= 0x0400;
            if (isdigit_l(c,host)) bits |= 0x0800;
            if (isxdigit_l(c,host)) bits |= 0x1000;
            if (isspace_l(c,host)) bits |= 0x2000;
            if (isprint_l(c,host)) bits |= 0x4000;
            if (isgraph_l(c,host)) bits |= 0x8000;
            if (isblank_l(c,host)) bits |= 0x0001;
            if (iscntrl_l(c,host)) bits |= 0x0002;
            if (ispunct_l(c,host)) bits |= 0x0004;
            if (isalnum_l(c,host)) bits |= 0x0008;
        }
        classes[i + 128] = (unsigned short)bits;
        lower[i + 128] = c < 0 || (byte_only && c>=128) ? c : tolower_l(c,host);
        upper[i + 128] = c < 0 || (byte_only && c>=128) ? c : toupper_l(c,host);
    }
    *(uintptr_t *)(locale + 0x68) = (uintptr_t)(classes + 128);
    *(uintptr_t *)(locale + 0x70) = (uintptr_t)(lower + 128);
    *(uintptr_t *)(locale + 0x78) = (uintptr_t)(upper + 128);
    for(unsigned i=0;i<13;++i)*(uintptr_t *)(locale+0x80+i*sizeof(uintptr_t))=(uintptr_t)guest_name;
    // glibc's 13 __names pointers occupy 0x80..0xe7.
    *(uintptr_t *)(locale + 0xe8) = (uintptr_t)host;
    return (uintptr_t)locale;
}

static locale_t host_locale(uint64_t value) {
    if(value==UINT64_MAX)return LC_GLOBAL_LOCALE;
    return value ? (locale_t)*(uintptr_t *)((uint8_t *)(uintptr_t)value + 0xe8) : (locale_t)0;
}
static void guest_freelocale(Guest *g,uint64_t value) {
    if(!value || value==UINT64_MAX)return;
    uint8_t *locale=(uint8_t *)(uintptr_t)value;locale_t host=host_locale(value);
    guest_free(g,(unsigned short *)(uintptr_t)*(uintptr_t *)(locale+0x68)-128);
    guest_free(g,(int *)(uintptr_t)*(uintptr_t *)(locale+0x70)-128);
    guest_free(g,(int *)(uintptr_t)*(uintptr_t *)(locale+0x78)-128);
    guest_free(g,(void *)(uintptr_t)*(uintptr_t *)(locale+0x80));
    guest_free(g,locale);if(host)freelocale(host);
}

static int write_path_allowed(const char *path) {
    char root[PATH_MAX], parent[PATH_MAX], resolved[PATH_MAX];
    const char *data=getenv("GTTS_DATA");
    if (!data || !realpath(data, root) || !path || !*path || strlen(path) >= sizeof(parent)) return 0;
    struct stat st;
    if (!lstat(path, &st) && (S_ISLNK(st.st_mode) || st.st_nlink > 1)) return 0;
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (!slash) strcpy(parent, ".");
    else if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
    if (!realpath(parent, resolved)) return 0;
    size_t n = strlen(root);
    return !strncmp(root, resolved, n) && (resolved[n] == '\0' || resolved[n] == '/');
}

typedef struct {
    uint64_t dev, ino;
    uint32_t mode, nlink, uid, gid;
    uint64_t rdev, pad1;
    int64_t size;
    int32_t blksize, pad2;
    int64_t blocks, atime_sec;
    uint64_t atime_nsec;
    int64_t mtime_sec;
    uint64_t mtime_nsec;
    int64_t ctime_sec;
    uint64_t ctime_nsec;
    uint32_t unused4, unused5;
} LinuxStat;
_Static_assert(sizeof(LinuxStat) == 128, "Linux aarch64 stat size");
_Static_assert(offsetof(LinuxStat, size) == 48, "Linux aarch64 stat size offset");

static uint64_t guest_stat(Guest *g, const char *name, const uint64_t a[8]) {
    struct stat s;
    int r;
    if (!strcmp(name, "fstat") || !strcmp(name, "fstat64")) r = fstat((int)a[0], &s);
    else if (!strcmp(name, "lstat") || !strcmp(name, "lstat64")) r = lstat((const char *)(uintptr_t)a[0], &s);
    else r = stat((const char *)(uintptr_t)a[0], &s);
    if (r) { *(int *)(uintptr_t)g->errno_slot = linux_errno(errno); return UINT64_MAX; }
    LinuxStat out = {0};
    out.dev = s.st_dev; out.ino = s.st_ino; out.mode = s.st_mode; out.nlink = s.st_nlink;
    out.uid = s.st_uid; out.gid = s.st_gid; out.rdev = s.st_rdev; out.size = s.st_size;
    out.blksize = s.st_blksize; out.blocks = s.st_blocks;
    out.atime_sec = s.st_atimespec.tv_sec; out.atime_nsec = s.st_atimespec.tv_nsec;
    out.mtime_sec = s.st_mtimespec.tv_sec; out.mtime_nsec = s.st_mtimespec.tv_nsec;
    out.ctime_sec = s.st_ctimespec.tv_sec; out.ctime_nsec = s.st_ctimespec.tv_nsec;
    memcpy((void *)(uintptr_t)a[1], &out, sizeof out);
    return 0;
}

static int linux_open_flags(uint64_t flags) {
    int out = (int)(flags & 3);
    if (flags & 0x40) out |= O_CREAT;
    if (flags & 0x80) out |= O_EXCL;
    if (flags & 0x200) out |= O_TRUNC;
    if (flags & 0x400) out |= O_APPEND;
    if (flags & 0x800) out |= O_NONBLOCK;
    if (flags & 0x4000) out |= O_DIRECTORY;
    if (flags & 0x8000) out |= O_NOFOLLOW;
    if (flags & 0x80000) out |= O_CLOEXEC;
    return out;
}

static int virtual_cpu_file(const char *path) {
    const char *content = NULL;
    if (!strcmp(path, "/sys/devices/system/cpu/possible") ||
        !strcmp(path, "/sys/devices/system/cpu/present") ||
        !strcmp(path, "/sys/devices/system/cpu/online")) content = "0-0\n";
    else if (!strcmp(path, "/sys/devices/system/cpu/kernel_max")) content = "0\n";
    else if (!strcmp(path, "/proc/cpuinfo")) content =
        "processor\t: 0\n"
        "BogoMIPS\t: 100.00\n"
        "Features\t: fp asimd\n"
        "CPU implementer\t: 0x61\n"
        "CPU architecture: 8\n"
        "CPU variant\t: 0x0\n"
        "CPU part\t: 0x000\n"
        "CPU revision\t: 0\n\n";
    if (!content) return -2;
    int fds[2];
    if (pipe(fds)) return -1;
    size_t length = strlen(content);
    ssize_t written = write(fds[1], content, length);
    close(fds[1]);
    if (written != (ssize_t)length) { close(fds[0]); return -1; }
    return fds[0];
}

typedef struct MapRange {uint64_t begin,end;struct MapRange *next;} MapRange;
typedef struct GuestMapping {void *allocation;MapRange *ranges;struct GuestMapping *next;} GuestMapping;

void guest_mappings_destroy(Guest *g) {
    GuestMapping *mapping=g->master->mappings;
    while(mapping) {GuestMapping *next=mapping->next;MapRange *range=mapping->ranges;
        while(range){MapRange *following=range->next;free(range);range=following;}free(mapping);mapping=next;}
    g->master->mappings=NULL;
}
static uint64_t guest_munmap(Guest *g,uint64_t address,uint64_t length) {
    if(!length || (address&4095) || length>UINT64_MAX-4095 || address>UINT64_MAX-((length+4095)&~UINT64_C(4095))) {
        *(int *)(uintptr_t)g->errno_slot=22;return UINT64_MAX;
    }
    uint64_t end=address+((length+4095)&~UINT64_C(4095));Guest *shared=g->master;
    pthread_mutex_lock(&shared->map_lock);
    GuestMapping **mp=(GuestMapping **)&shared->mappings;
    while(*mp) {
        GuestMapping *mapping=*mp;MapRange **rp=&mapping->ranges;
        while(*rp) {
            MapRange *range=*rp;
            if(end<=range->begin || address>=range->end){rp=&range->next;continue;}
            if(address<=range->begin && end>=range->end){*rp=range->next;free(range);continue;}
            if(address<=range->begin){range->begin=end;rp=&range->next;continue;}
            if(end>=range->end){range->end=address;rp=&range->next;continue;}
            MapRange *tail=malloc(sizeof *tail);
            if(!tail){pthread_mutex_unlock(&shared->map_lock);*(int *)(uintptr_t)g->errno_slot=12;return UINT64_MAX;}
            *tail=(MapRange){end,range->end,range->next};range->end=address;range->next=tail;rp=&tail->next;
        }
        if(!mapping->ranges){*mp=mapping->next;guest_free(g,mapping->allocation);free(mapping);}
        else mp=&mapping->next;
    }
    pthread_mutex_unlock(&shared->map_lock);return 0;
}

static uint64_t guest_mmap(Guest *g, const uint64_t a[6]) {
    uint64_t address = a[0], length = a[1], flags = a[3];
    if (g->native_mode && (a[2] & 4)) { *(int *)(uintptr_t)g->errno_slot=95; return UINT64_MAX; }
    if (!length || length > g->arena_size || length > SIZE_MAX-16383) {
        *(int *)(uintptr_t)g->errno_slot = ENOMEM; return UINT64_MAX;
    }
    if(flags&0x10){(void)address;*(int *)(uintptr_t)g->errno_slot=95;return UINT64_MAX;} // MAP_FIXED cannot safely replace arbitrary arena storage.
    size_t mapped=((size_t)length+4095)&~(size_t)4095;
    size_t allocation=((size_t)length+16383)&~(size_t)16383;
    void *p=guest_aligned_malloc(g,16384,allocation);
    GuestMapping *mapping=malloc(sizeof *mapping);MapRange *range=malloc(sizeof *range);
    if(!p || !mapping || !range){guest_free(g,p);free(mapping);free(range);*(int *)(uintptr_t)g->errno_slot=12;return UINT64_MAX;}
    memset(p,0,allocation);
    if (!(flags & 0x20)) { // Linux MAP_ANONYMOUS
        size_t total = 0;
        while (total < length) {
            ssize_t n = pread((int)a[4], (uint8_t *)p + total, (size_t)length - total, (off_t)(a[5] + total));
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) { *(int *)(uintptr_t)g->errno_slot = linux_errno(errno);guest_free(g,p);free(mapping);free(range);return UINT64_MAX; }
            if (!n) break;
            total += (size_t)n;
        }
        if (total < length) memset((uint8_t *)p + total, 0, (size_t)length - total);
    }
    *range=(MapRange){(uintptr_t)p,(uintptr_t)p+mapped,NULL};
    Guest *shared=g->master;pthread_mutex_lock(&shared->map_lock);
    *mapping=(GuestMapping){p,range,shared->mappings};shared->mappings=mapping;
    pthread_mutex_unlock(&shared->map_lock);return (uintptr_t)p;
}

typedef struct { uint64_t stack, gr_top, vr_top; int32_t gr_offs, vr_offs; } LinuxVaList;
typedef union { uint64_t u; int64_t i; double d; } FfiValue;

static uint64_t linux_va_integer(LinuxVaList *va) {
    uint64_t address;
    if (va->gr_offs < 0) { address = va->gr_top + va->gr_offs; va->gr_offs += 8; }
    else { address = va->stack; va->stack += 8; }
    return *(uint64_t *)(uintptr_t)address;
}

static double linux_va_double(LinuxVaList *va) {
    uint64_t address;
    if (va->vr_offs < 0) { address = va->vr_top + va->vr_offs; va->vr_offs += 16; }
    else { address = va->stack; va->stack += 8; }
    double value;
    memcpy(&value, (void *)(uintptr_t)address, sizeof value);
    return value;
}

// Convert the Linux AArch64 va_list to a Darwin variadic call through libffi.
static int linux_vformat(Guest *g, const char *name, const uint64_t a[8], uint64_t *result) {
    unsigned fixed = 0, va_arg = 0, direct = 0;
    const char *format = NULL;
    char wide_format[8192];
    void *function = NULL;
    char *allocated = NULL;
    FfiValue values[96] = {{0}};
    ffi_type *types[96];
    void *arguments[96];
    if (!strcmp(name, "__vasprintf_chk")) {
        fixed = 2; va_arg = 3; format = (const char *)(uintptr_t)a[2]; function = (void *)asprintf;
        values[0].u = (uintptr_t)&allocated; values[1].u = a[2];
    } else if (!strcmp(name, "__vfprintf_chk")) {
        fixed = 2; va_arg = 3; format = (const char *)(uintptr_t)a[2]; function = (void *)fprintf;
        values[0].u = a[0]; values[1].u = a[2];
    } else if (!strcmp(name, "__vprintf_chk")) {
        fixed = 1; va_arg = 2; format = (const char *)(uintptr_t)a[1]; function = (void *)printf;
        values[0].u = a[1];
    } else if (!strcmp(name, "__vsnprintf_chk")) {
        fixed = 3; va_arg = 5; format = (const char *)(uintptr_t)a[4]; function = (void *)snprintf;
        values[0].u = a[0]; values[1].u = a[1]; values[2].u = a[4];
    } else if (!strcmp(name, "__vsprintf_chk")) {
        fixed = 3; va_arg = 4; format = (const char *)(uintptr_t)a[3]; function = (void *)snprintf;
        values[0].u = a[0]; values[1].u = a[2]; values[2].u = a[3];
    } else if(!strcmp(name,"snprintf")) {
        fixed=3;direct=3;format=(const char *)(uintptr_t)a[2];function=(void *)snprintf;
        values[0].u=a[0];values[1].u=a[1];values[2].u=a[2];
    } else if(!strcmp(name,"__snprintf_chk")) {
        if(a[1]>a[3])return -1;fixed=3;direct=5;format=(const char *)(uintptr_t)a[4];function=(void *)snprintf;
        values[0].u=a[0];values[1].u=a[1];values[2].u=a[4];
    } else if(!strcmp(name,"fprintf") || !strcmp(name,"__fprintf_chk")) {
        fixed=2;direct=!strcmp(name,"fprintf")?2:3;format=(const char *)(uintptr_t)a[direct-1];function=(void *)fprintf;
        values[0].u=a[0];values[1].u=a[direct-1];
    } else if(!strcmp(name,"printf") || !strcmp(name,"__printf_chk")) {
        fixed=1;direct=!strcmp(name,"printf")?1:2;format=(const char *)(uintptr_t)a[direct-1];function=(void *)printf;
        values[0].u=a[direct-1];
    } else if(!strcmp(name,"swprintf")) {
        const wchar_t *wide=(const wchar_t *)(uintptr_t)a[2];if(!wide)return -1;
        size_t n=wcslen(wide);if(n>=sizeof wide_format)return -1;
        for(size_t i=0;i<n;++i)wide_format[i]=wide[i]<128?(char)wide[i]:'?';wide_format[n]=0;
        fixed=3;direct=3;format=wide_format;function=(void *)swprintf;
        values[0].u=a[0];values[1].u=a[1];values[2].u=a[2];
    } else return 0;
    if (!format || (!direct && !a[va_arg])) return -1;
    if (g->trace) fprintf(stderr, "  Linux format: %s\n", format);
    LinuxVaList va;uint64_t gr[8];hv_simd_fp_uchar16_t vr[8];
    if(direct) {
        memcpy(gr,a,sizeof gr);for(unsigned i=0;i<8;++i)
            if(guest_get_simd(g,i,&vr[i]))return -1;
        uint64_t sp=0;if(guest_get_sp(g,&sp))return -1;
        va=(LinuxVaList){sp,(uintptr_t)(gr+8),(uintptr_t)(vr+8),(int32_t)((int)direct-8)*8,-128};
    } else memcpy(&va, (const void *)(uintptr_t)a[va_arg], sizeof va);
    for (unsigned i = 0; i < fixed; ++i) types[i] = (fixed == 3 && i == 1) ? &ffi_type_uint64 : &ffi_type_pointer;
    unsigned count = fixed;
#define NEXT_ARG(type_, expr_) do { if (count >= 96) return -1; types[count] = (type_); values[count].u = (expr_); ++count; } while (0)
    for (const char *p = format; *p; ++p) {
        if (*p != '%') continue;
        ++p;
        if (*p == '%') continue;
        if (!*p) return -1;
        while (strchr("-+ #0'", *p) && *p) ++p;
        if (*p == '*') { NEXT_ARG(&ffi_type_sint32, linux_va_integer(&va)); ++p; }
        else while (isdigit((unsigned char)*p)) ++p;
        if (*p == '$') { fprintf(stderr, "Positional printf arguments are not implemented\n"); return -1; }
        if (*p == '.') {
            ++p;
            if (*p == '*') { NEXT_ARG(&ffi_type_sint32, linux_va_integer(&va)); ++p; }
            else while (isdigit((unsigned char)*p)) ++p;
        }
        int wide = 0, long_double = 0;
        if (*p == 'h') { ++p; if (*p == 'h') ++p; }
        else if (*p == 'l') { wide = 1; ++p; if (*p == 'l') ++p; }
        else if (strchr("jzt", *p) && *p) { wide = 1; ++p; }
        else if (*p == 'L') { long_double = 1; ++p; }
        if (!*p) return -1;
        if (strchr("di", *p)) NEXT_ARG(wide ? &ffi_type_sint64 : &ffi_type_sint32, linux_va_integer(&va));
        else if (strchr("ouxX", *p)) NEXT_ARG(wide ? &ffi_type_uint64 : &ffi_type_uint32, linux_va_integer(&va));
        else if (*p == 'c') NEXT_ARG(&ffi_type_sint32, linux_va_integer(&va));
        else if (strchr("spn", *p)) NEXT_ARG(&ffi_type_pointer, linux_va_integer(&va));
        else if (strchr("aAeEfFgG", *p)) {
            if (long_double || count >= 96) { fprintf(stderr, "Unsupported long double format\n"); return -1; }
            types[count] = &ffi_type_double; values[count].d = linux_va_double(&va); ++count;
        } else if (*p != 'm') { fprintf(stderr, "Unsupported printf conversion %%%c\n", *p); return -1; }
    }
#undef NEXT_ARG
    for (unsigned i = 0; i < count; ++i) arguments[i] = &values[i];
    ffi_cif cif;
    if (ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, fixed, count, &ffi_type_sint32, types) != FFI_OK) return -1;
    ffi_sarg written = -1;
    errno=host_errno(*(int *)(uintptr_t)g->errno_slot);
    ffi_call(&cif, FFI_FN(function), &written, arguments);
    *(int *)(uintptr_t)g->errno_slot=linux_errno(errno);
    if (!strcmp(name, "__vasprintf_chk")) {
        if (written >= 0 && allocated) {
            void *copy = guest_malloc(g, (size_t)written + 1);
            if (!copy) { free(allocated); return -1; }
            memcpy(copy, allocated, (size_t)written + 1);
            *(uintptr_t *)(uintptr_t)a[0] = (uintptr_t)copy;
        }
        free(allocated);
    }
    *result = (uint64_t)(int64_t)written;
    return 1;
}

static void scan_integer_store(void *dst,int length,int unsign,uint64_t value) {
    if(unsign) {
        if(length==-2)*(unsigned char *)dst=(unsigned char)value;
        else if(length==-1)*(unsigned short *)dst=(unsigned short)value;
        else if(!length)*(unsigned int *)dst=(unsigned int)value;
        else *(uint64_t *)dst=value;
    } else {
        if(length==-2)*(signed char *)dst=(signed char)value;
        else if(length==-1)*(short *)dst=(short)value;
        else if(!length)*(int *)dst=(int)value;
        else *(int64_t *)dst=(int64_t)value;
    }
}
static int linux_c23_scan(Guest *g,const char *input,const char *format,LinuxVaList va,uint64_t *result) {
    const char *cursor=input,*p=format;int assigned=0;
    while(*p) {
        if(isspace((unsigned char)*p)){while(isspace((unsigned char)*p))++p;while(isspace((unsigned char)*cursor))++cursor;continue;}
        if(*p!='%'){if(*cursor!=*p){*result=(uint64_t)(int64_t)(!*cursor && !assigned?-1:assigned);return 1;}++p;++cursor;continue;}
        const char *start=p++;if(*p=='%'){if(*cursor!='%'){*result=(uint64_t)(int64_t)(!*cursor && !assigned?-1:assigned);return 1;}++p;++cursor;continue;}
        int suppress=0;if(*p=='*'){suppress=1;++p;}unsigned width=0;
        while(isdigit((unsigned char)*p)){if(width>1000000)return -1;width=width*10+(unsigned)(*p++-'0');}
        if(*p=='$' || *p=='m' || *p=='L')return -1;
        int length=0;if(*p=='h'){length=-1;++p;if(*p=='h'){length=-2;++p;}}
        else if(*p=='l'){length=1;++p;if(*p=='l'){length=2;++p;}}
        else if(strchr("jzt",*p) && *p){length=2;++p;}
        char conversion=*p;if(!conversion || !strchr("diouxXbBfFeEgGaAcspn[",conversion))return -1;
        if(conversion=='['){++p;if(*p=='^')++p;if(*p==']')++p;while(*p && *p!=']')++p;if(!*p)return -1;}
        ++p;size_t spec_length=(size_t)(p-start);if(spec_length+3>=256)return -1;
        void *dest=suppress?NULL:(void *)(uintptr_t)linux_va_integer(&va);
        if(conversion=='n'){if(!suppress)scan_integer_store(dest,length,0,(uint64_t)(cursor-input));continue;}
        if(conversion=='i' || conversion=='b' || conversion=='B') {
            const char *number=cursor;while(isspace((unsigned char)*number))++number;
            const char *q=number;unsigned left=width?width:UINT_MAX;int negative=0;
            if(left && (*q=='+' || *q=='-')){negative=*q=='-';++q;--left;}
            int prefix=left>=2 && q[0]=='0' && (q[1]=='b' || q[1]=='B');
            if(prefix || conversion=='b' || conversion=='B') {
                if(prefix){q+=2;left-=2;}const char *digits=q;uint64_t value=0;int overflow=0;
                while(left && (*q=='0' || *q=='1')){unsigned digit=(unsigned)(*q-'0');
                    if(value>(UINT64_MAX-digit)/2)overflow=1;else value=value*2+digit;++q;--left;}
                if(q!=digits || prefix){int unsign=conversion!='i';
                    if(overflow || (!unsign && value>(negative?UINT64_C(0x8000000000000000):INT64_MAX))){
                        *(int *)(uintptr_t)g->errno_slot=34;value=unsign?UINT64_MAX:(negative?UINT64_C(0x8000000000000000):INT64_MAX);
                    } else if(negative)value=0-value;
                    if(!suppress){scan_integer_store(dest,length,unsign,value);++assigned;}cursor=q;continue;
                }
                *result=(uint64_t)(int64_t)(!*number && !assigned?-1:assigned);return 1;
            }
        }
        char spec[256];memcpy(spec,start,spec_length);memcpy(spec+spec_length,"%n",3);int used=-1;
        errno=host_errno(*(int *)(uintptr_t)g->errno_slot);
        if(suppress)(void)sscanf(cursor,spec,&used);else (void)sscanf(cursor,spec,dest,&used);
        *(int *)(uintptr_t)g->errno_slot=linux_errno(errno);
        if(used<0){const char *q=cursor;if(conversion!='c' && conversion!='[')while(isspace((unsigned char)*q))++q;
            *result=(uint64_t)(int64_t)(!*q && !assigned?-1:assigned);return 1;}
        cursor+=used;if(!suppress)++assigned;
    }
    *result=(uint64_t)(int64_t)assigned;return 1;
}

static int linux_vscan(Guest *g,const char *name,const uint64_t a[8],uint64_t *result) {
    int isva=!strcmp(name,"__isoc23_vsscanf") || !strcmp(name,"__isoc99_vsscanf");
    int direct=!strcmp(name,"__isoc23_sscanf") || !strcmp(name,"__isoc99_sscanf") || !strcmp(name,"sscanf");
    if(!isva && !direct)return 0;
    const char *input=(const char *)(uintptr_t)a[0],*format=(const char *)(uintptr_t)a[1];
    if(!input || !format || (isva && !a[2]))return -1;
    LinuxVaList va;uint64_t gr[8];
    if(isva)memcpy(&va,(void *)(uintptr_t)a[2],sizeof va);
    else {uint64_t sp=0;memcpy(gr,a,sizeof gr);
        if(guest_get_sp(g,&sp))return -1;
        va=(LinuxVaList){sp,(uintptr_t)(gr+8),0,-48,0};}
    if(strstr(name,"isoc23") && (strstr(input,"0b") || strstr(input,"0B") || strchr(format,'b') || strchr(format,'B')))
        return linux_c23_scan(g,input,format,va,result);
    ffi_type *types[96];void *args[96];FfiValue values[96]={{0}};
    values[0].u=a[0];values[1].u=a[1];types[0]=types[1]=&ffi_type_pointer;unsigned count=2;
    for(const char *p=format;*p;++p){
        if(*p!='%')continue;if(!*++p)return -1;if(*p=='%')continue;
        int suppress=0;if(*p=='*'){suppress=1;++p;}
        while(isdigit((unsigned char)*p))++p;
        if(*p=='$' || *p=='m' || *p=='L')return -1;
        if(*p=='h' || *p=='l'){char c=*p++;if(*p==c)++p;}
        else if(strchr("jzt",*p) && *p)++p;
        if(!*p || !strchr("diouxXfFeEgGaAcspn[",*p))return -1;
        if(*p=='['){++p;if(*p=='^')++p;if(*p==']')++p;while(*p && *p!=']')++p;if(!*p)return -1;}
        if(!suppress){if(count>=96)return -1;types[count]=&ffi_type_pointer;values[count++].u=linux_va_integer(&va);}
    }
    for(unsigned i=0;i<count;++i)args[i]=&values[i];ffi_cif cif;ffi_sarg scanned=-1;
    if(ffi_prep_cif_var(&cif,FFI_DEFAULT_ABI,2,count,&ffi_type_sint32,types)!=FFI_OK)return -1;
    errno=host_errno(*(int *)(uintptr_t)g->errno_slot);ffi_call(&cif,FFI_FN(sscanf),&scanned,args);
    *(int *)(uintptr_t)g->errno_slot=linux_errno(errno);*result=(uint64_t)scanned;return 1;
}

static int c23_integer(Guest *g,const char *name,const uint64_t a[8],uint64_t *result) {
    if(strncmp(name,"__isoc23_",9))return 0;
    const char *kind=name+9;
    static const char *const integers[]={"strtol","strtoll","strtoul","strtoull","strtoll_l","strtoull_l",
        "wcstol","wcstoll","wcstoul","wcstoull",NULL};
    int valid=0;for(size_t i=0;integers[i];++i)if(!strcmp(kind,integers[i])){valid=1;break;}if(!valid)return 0;
    int wide=!strncmp(kind,"wcsto",5),narrow=!strncmp(kind,"strto",5);
    if(!wide && !narrow)return 0;
    int unsign=wide?!strncmp(kind,"wcstou",6):!strncmp(kind,"strtou",6);
    int use_locale=strstr(kind,"_l")!=NULL,base=(int)a[2];locale_t loc=use_locale?host_locale(a[3]):(locale_t)0;
    int negative=0,binary=0;const char *p=NULL;const wchar_t *wp=NULL;
    if(wide) {
        wp=(const wchar_t *)(uintptr_t)a[0];while(iswspace(*wp))++wp;
        if(*wp==L'+' || *wp==L'-'){negative=*wp==L'-';++wp;}
        binary=(base==0 || base==2) && wp[0]==L'0' && (wp[1]==L'b' || wp[1]==L'B') && (wp[2]==L'0' || wp[2]==L'1');
    } else {
        p=(const char *)(uintptr_t)a[0];while(use_locale?isspace_l((unsigned char)*p,loc):isspace((unsigned char)*p))++p;
        if(*p=='+' || *p=='-'){negative=*p=='-';++p;}
        binary=(base==0 || base==2) && p[0]=='0' && (p[1]=='b' || p[1]=='B') && (p[2]=='0' || p[2]=='1');
    }
    if(binary) {
        errno=0;unsigned long long magnitude;
        if(wide){wchar_t *end=NULL;magnitude=wcstoull(wp+2,&end,2);if(a[1])*(wchar_t **)(uintptr_t)a[1]=end;}
        else {char *end=NULL;magnitude=strtoull(p+2,&end,2);if(a[1])*(char **)(uintptr_t)a[1]=end;}
        uint64_t limit=negative?UINT64_C(0x8000000000000000):INT64_MAX;
        if(errno==ERANGE || (!unsign && magnitude>limit)){
            *(int *)(uintptr_t)g->errno_slot=34;*result=unsign?UINT64_MAX:(negative?UINT64_C(0x8000000000000000):INT64_MAX);
        } else *result=negative?0-(uint64_t)magnitude:(uint64_t)magnitude;
        return 1;
    }
    errno=host_errno(*(int *)(uintptr_t)g->errno_slot);
    if(wide) *result=unsign?wcstoull((const wchar_t *)(uintptr_t)a[0],(wchar_t **)(uintptr_t)a[1],base):
                          (uint64_t)wcstoll((const wchar_t *)(uintptr_t)a[0],(wchar_t **)(uintptr_t)a[1],base);
    else if(use_locale)*result=unsign?strtoull_l((const char *)(uintptr_t)a[0],(char **)(uintptr_t)a[1],base,loc):
                                   (uint64_t)strtoll_l((const char *)(uintptr_t)a[0],(char **)(uintptr_t)a[1],base,loc);
    else *result=unsign?strtoull((const char *)(uintptr_t)a[0],(char **)(uintptr_t)a[1],base):
                        (uint64_t)strtoll((const char *)(uintptr_t)a[0],(char **)(uintptr_t)a[1],base);
    *(int *)(uintptr_t)g->errno_slot=linux_errno(errno);return 1;
}

static int in_names(const char *name, const char *const *names) {
    for (size_t i = 0; names[i]; ++i) if (!strcmp(name, names[i])) return 1;
    return 0;
}

static int multibyte_call(Guest *g,const char *name,const uint64_t a[8],uint64_t *result) {
    static const char *const names[]={"mbrtowc","mbrlen","wcrtomb","mbsrtowcs","mbsnrtowcs","wcsrtombs","wcsnrtombs","mbsinit",NULL};
    if(!in_names(name,names))return 0;
    unsigned index=(!strcmp(name,"mbrlen") || !strcmp(name,"wcrtomb"))?2:
                   (!strcmp(name,"mbsnrtowcs") || !strcmp(name,"wcsnrtombs"))?4:
                   !strcmp(name,"mbsinit")?0:3;
    mbstate_t state={0};mbstate_t *host=NULL;
    if(a[index]){memcpy(&state,(void *)(uintptr_t)a[index],8);host=&state;}
    errno=host_errno(*(int *)(uintptr_t)g->errno_slot);size_t value=0;
    if(!strcmp(name,"mbrtowc"))value=mbrtowc((wchar_t *)(uintptr_t)a[0],(const char *)(uintptr_t)a[1],(size_t)a[2],host);
    else if(!strcmp(name,"mbrlen"))value=mbrlen((const char *)(uintptr_t)a[0],(size_t)a[1],host);
    else if(!strcmp(name,"wcrtomb"))value=wcrtomb((char *)(uintptr_t)a[0],(wchar_t)a[1],host);
    else if(!strcmp(name,"mbsrtowcs"))value=mbsrtowcs((wchar_t *)(uintptr_t)a[0],(const char **)(uintptr_t)a[1],(size_t)a[2],host);
    else if(!strcmp(name,"mbsnrtowcs"))value=mbsnrtowcs((wchar_t *)(uintptr_t)a[0],(const char **)(uintptr_t)a[1],(size_t)a[2],(size_t)a[3],host);
    else if(!strcmp(name,"wcsrtombs"))value=wcsrtombs((char *)(uintptr_t)a[0],(const wchar_t **)(uintptr_t)a[1],(size_t)a[2],host);
    else if(!strcmp(name,"wcsnrtombs"))value=wcsnrtombs((char *)(uintptr_t)a[0],(const wchar_t **)(uintptr_t)a[1],(size_t)a[2],(size_t)a[3],host);
    else value=(size_t)mbsinit(host);
    *(int *)(uintptr_t)g->errno_slot=linux_errno(errno);
    if(host){const unsigned char *bytes=(const unsigned char *)&state;
        for(size_t i=8;i<sizeof state;++i)if(bytes[i]){fprintf(stderr,"Unsupported multibyte state for %s\n",name);return -1;}
        memcpy((void *)(uintptr_t)a[index],&state,8);}
    *result=(uint64_t)value;return 1;
}

static int quad_call(Guest *g,const char *name,const uint64_t a[8]) {
    int wide=!strcmp(name,"wcstold"),localized=!strcmp(name,"strtold_l");
    if(!wide && !localized && strcmp(name,"strtold"))return 0;
    locale_t locale=localized?host_locale(a[2]):uselocale(NULL);
    struct lconv *lc=locale==LC_GLOBAL_LOCALE?localeconv():localeconv_l(locale);
    const char *decimal=lc && lc->decimal_point?lc->decimal_point:".";
    const char *input=NULL,*end=NULL;char *owned=NULL;const wchar_t *wide_end=NULL;
    if(wide) {
        const wchar_t *source=(const wchar_t *)(uintptr_t)a[0];wchar_t *position=NULL;
        (void)wcstod(source,&position);wide_end=position;
        size_t length=(size_t)(position-source);if(length>20000)return -1;
        wchar_t *copy=calloc(length+1,sizeof *copy);if(!copy)return -1;memcpy(copy,source,length*sizeof *copy);
        size_t bytes=wcstombs(NULL,copy,0);if(bytes==(size_t)-1){free(copy);return -1;}
        owned=malloc(bytes+1);if(!owned){free(copy);return -1;}
        if(wcstombs(owned,copy,bytes+1)==(size_t)-1){free(copy);free(owned);return -1;}
        free(copy);input=owned;end=owned+bytes;
    } else {
        input=(const char *)(uintptr_t)a[0];char *position=NULL;
        if(localized)(void)strtod_l(input,&position,locale);else (void)strtod(input,&position);
        end=position;
    }
    const char *number=input;
    while(number<end && (localized?isspace_l((unsigned char)*number,locale):isspace((unsigned char)*number)))++number;
    uint64_t fpcr=0,bits[2]={0};int range_error=0;
    int failed=guest_get_fpcr(g,&fpcr) ||
        guest_quad_parse(number,end,decimal,(unsigned)((fpcr>>22)&3),bits,&range_error);
    if(!failed && a[1]) {
        if(wide)*(const wchar_t **)(uintptr_t)a[1]=wide_end;
        else *(const char **)(uintptr_t)a[1]=end;
    }
    free(owned);if(failed){fprintf(stderr,"Unsupported binary128 input for %s\n",name);return -1;}
    if(range_error)*(int *)(uintptr_t)g->errno_slot=34;
    hv_simd_fp_uchar16_t value={0};memcpy(&value,bits,sizeof bits);
    return guest_set_simd(g,0,value)==0?1:-1;
}

static int floating_call(Guest *g, const char *name, const uint64_t a[8], uint64_t *integer_result) {
    static const char *const unary_double[] = {"log", "log2", "log10", "log1p", "exp", "sqrt", "cos", "sin", "tan", "atan", "asin", "erf", "nan", NULL};
    static const char *const binary_double[] = {"pow", "atan2", "fmod", "hypot", NULL};
    static const char *const unary_float[] = {"logf", "log2f", "log10f", "expf", "expm1f", "sqrtf", "cosf", "sinf", "tanhf", "erfcf", "cbrtf", "nanf", NULL};
    static const char *const binary_float[] = {"powf", "atan2f", "fmodf", "hypotf", "nextafterf", NULL};
    int ud = in_names(name, unary_double), bd = in_names(name, binary_double);
    int uf = in_names(name, unary_float), bf = in_names(name, binary_float);
    int special = !strcmp(name, "frexp") || !strcmp(name, "ldexp") || !strcmp(name, "ldexpf") ||
                  !strcmp(name, "sincos") || !strcmp(name, "sincosf") || !strcmp(name, "lrintf") ||
                  !strcmp(name, "strtod") || !strcmp(name, "strtod_l") || !strcmp(name, "strtof") ||
                  !strcmp(name, "strtof_l") || !strcmp(name, "modf") || !strcmp(name, "wcstod") || !strcmp(name, "wcstof");
    if (!ud && !bd && !uf && !bf && !special) return 0;
    hv_simd_fp_uchar16_t q0, q1, result = {0};
    if (guest_get_simd(g, 0, &q0) || guest_get_simd(g, 1, &q1)) return -1;
    double x = 0, y = 0, d = 0;
    float xf = 0, yf = 0, f = 0;
    memcpy(&x, &q0, sizeof x); memcpy(&y, &q1, sizeof y);
    memcpy(&xf, &q0, sizeof xf); memcpy(&yf, &q1, sizeof yf);
    void *fn = dlsym(RTLD_DEFAULT, name);
    errno = host_errno(*(int *)(uintptr_t)g->errno_slot);
    if (ud && strcmp(name, "nan")) d = ((double (*)(double))fn)(x);
    else if (bd) d = ((double (*)(double, double))fn)(x, y);
    else if (uf && strcmp(name, "nanf")) f = ((float (*)(float))fn)(xf);
    else if (bf) f = ((float (*)(float, float))fn)(xf, yf);
    else if (!strcmp(name, "nan")) d = nan((const char *)(uintptr_t)a[0]);
    else if (!strcmp(name, "nanf")) f = nanf((const char *)(uintptr_t)a[0]);
    else if (!strcmp(name, "frexp")) d = frexp(x, (int *)(uintptr_t)a[0]);
    else if (!strcmp(name, "modf")) d = modf(x,(double *)(uintptr_t)a[0]);
    else if (!strcmp(name, "ldexp")) d = ldexp(x, (int)a[0]);
    else if (!strcmp(name, "ldexpf")) f = ldexpf(xf, (int)a[0]);
    else if (!strcmp(name, "sincos")) { *(double *)(uintptr_t)a[0] = sin(x); *(double *)(uintptr_t)a[1] = cos(x); }
    else if (!strcmp(name, "sincosf")) { *(float *)(uintptr_t)a[0] = sinf(xf); *(float *)(uintptr_t)a[1] = cosf(xf); }
    else if (!strcmp(name, "lrintf")) { *integer_result = (uint64_t)lrintf(xf); *(int *)(uintptr_t)g->errno_slot = linux_errno(errno); return 1; }
    else if (!strcmp(name, "strtod")) d = strtod((const char *)(uintptr_t)a[0], (char **)(uintptr_t)a[1]);
    else if (!strcmp(name, "strtod_l")) d = strtod_l((const char *)(uintptr_t)a[0], (char **)(uintptr_t)a[1], host_locale(a[2]));
    else if (!strcmp(name, "strtof")) f = strtof((const char *)(uintptr_t)a[0], (char **)(uintptr_t)a[1]);
    else if (!strcmp(name, "strtof_l")) f = strtof_l((const char *)(uintptr_t)a[0], (char **)(uintptr_t)a[1], host_locale(a[2]));
    else if (!strcmp(name, "wcstod")) d = wcstod((const wchar_t *)(uintptr_t)a[0],(wchar_t **)(uintptr_t)a[1]);
    else if (!strcmp(name, "wcstof")) f = wcstof((const wchar_t *)(uintptr_t)a[0],(wchar_t **)(uintptr_t)a[1]);
    *(int *)(uintptr_t)g->errno_slot = linux_errno(errno);
    if (uf || bf || !strcmp(name, "ldexpf") || !strcmp(name, "strtof") || !strcmp(name, "strtof_l") || !strcmp(name, "wcstof"))
        memcpy(&result, &f, sizeof f);
    else memcpy(&result, &d, sizeof d);
    if (guest_set_simd(g, 0, result)) return -1;
    return 1;
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

static void *guest_aligned_malloc(Guest *g, size_t alignment, size_t size) {
    if (g->native_mode) return guest_native_heap_alloc(g, alignment, size);
    if (alignment < 16) alignment = 16;
    if (size < 8) size = 8;
    if (size > g->master->arena_size) return NULL;
    if (alignment > SIZE_MAX - 16 || size > SIZE_MAX - 16 - alignment) return NULL;
    size_t capacity = 0, bin = allocation_class(size, &capacity);
    if (bin >= 512 || capacity > SIZE_MAX - 16 - alignment) return NULL;
    Guest *shared = g->master;
    pthread_mutex_lock(&shared->heap_lock);
    size_t end = bin + 8 < 512 ? bin + 8 : 512;
    for (size_t i = bin; i < end; ++i) {
        uint64_t *previous = &shared->free_bins[i];
        while (*previous) {
            uint64_t *header = (uint64_t *)(uintptr_t)*previous;
            uint64_t *next = header + 2;
            if (header[0] >= size && !((uintptr_t)next & (alignment - 1))) {
                *previous = *next;
                header[1] = UINT64_C(0x4754545348454150);
                pthread_mutex_unlock(&shared->heap_lock);
                return next;
            }
            previous = next;
        }
    }
    uint8_t *raw = guest_alloc(g, capacity + 16 + alignment - 1, 16);
    if (!raw) { pthread_mutex_unlock(&shared->heap_lock); return NULL; }
    uintptr_t address = ((uintptr_t)raw + 16 + alignment - 1) & ~(alignment - 1);
    uint64_t *header = (uint64_t *)address - 2;
    header[0] = capacity;
    header[1] = UINT64_C(0x4754545348454150);
    pthread_mutex_unlock(&shared->heap_lock);
    return (void *)address;
}

static void *guest_malloc(Guest *g, size_t size) {
    return guest_aligned_malloc(g, 16, size);
}

static void guest_free(Guest *g, void *pointer) {
    if (!pointer) return;
    if (g->native_mode) { guest_native_heap_free(g, pointer); return; }
    Guest *shared = g->master;
    pthread_mutex_lock(&shared->heap_lock);
    uint64_t *header = (uint64_t *)pointer - 2;
    if (header[1] != UINT64_C(0x4754545348454150)) {
        fprintf(stderr, "invalid guest free: %p\n", pointer);
        pthread_mutex_unlock(&shared->heap_lock);
        return;
    }
    header[1] = 0;
    size_t capacity = 0, bin = allocation_class((size_t)header[0], &capacity);
    if (bin >= 512) { pthread_mutex_unlock(&shared->heap_lock); return; }
    *(uint64_t *)pointer = shared->free_bins[bin];
    shared->free_bins[bin] = (uintptr_t)header;
    pthread_mutex_unlock(&shared->heap_lock);
}

void *guest_heap_alloc(Guest *g, size_t size) { return guest_malloc(g, size); }
void *guest_heap_aligned_alloc(Guest *g, size_t alignment,size_t size) {return guest_aligned_malloc(g,alignment,size);}
void guest_heap_free(Guest *g, void *pointer) { guest_free(g, pointer); }

typedef struct ThreadDestructor {uint64_t function,argument;struct ThreadDestructor *next;} ThreadDestructor;
typedef struct ExitDestructor {uint64_t function,argument,dso;struct ExitDestructor *next;} ExitDestructor;
int guest_run_exit_destructors(Guest *g, uint64_t dso, int discard) {
    Guest *shared = g->master;
    for (;;) {
        pthread_mutex_lock(&shared->once_lock);
        ExitDestructor **next = (ExitDestructor **)&shared->exit_destructors;
        while (*next && dso && (*next)->dso != dso) next = &(*next)->next;
        ExitDestructor *entry = *next;
        if (entry) *next = entry->next;
        pthread_mutex_unlock(&shared->once_lock);
        if (!entry) return discard ? -1 : 0;
        if (!discard) {
            uint64_t args[8] = {entry->argument}, ignored = 0;
            if (guest_callback(g, entry->function, args, &ignored)) discard = 1;
        }
        free(entry);
    }
}
void guest_run_thread_destructors(Guest *g) {
    while(g->thread_destructors) {
        ThreadDestructor *d=g->thread_destructors;g->thread_destructors=d->next;
        uint64_t args[8]={d->argument},ignored=0;
        int failed=guest_callback(g,d->function,args,&ignored);free(d);
        if(failed)break;
    }
    while(g->thread_destructors){ThreadDestructor *d=g->thread_destructors;g->thread_destructors=d->next;free(d);}
}

typedef struct { Guest guest; uint64_t entry, argument; pthread_mutex_t lock; pthread_cond_t cond; int ready, accepted, error; } GuestThread;

static void *guest_thread_main(void *opaque) {
    GuestThread *thread = opaque;
    Guest *g = &thread->guest, *shared = g->master;
    uint64_t result = 0;
    int error=guest_thread_cpu_init(g);
    pthread_mutex_lock(&thread->lock);thread->error=error;thread->ready=1;pthread_cond_signal(&thread->cond);
    while(!thread->accepted)pthread_cond_wait(&thread->cond,&thread->lock);
    pthread_mutex_unlock(&thread->lock);
    if (!error) {
        uint64_t args[8] = {thread->argument};
        if (guest_call(g, thread->entry, args, &result))
            fprintf(stderr, "Guest worker %u failed\n", g->tid);
    } else fprintf(stderr, "Could not initialize guest worker %u\n", g->tid);
    if(!error && !__atomic_load_n(&shared->broken,__ATOMIC_ACQUIRE))guest_run_thread_destructors(g);
    if(g->thread_vcpu_created)hv_vcpu_destroy(g->vcpu);
    guest_free(g,g->stack);guest_free(g,g->tls);guest_free(g,(void *)(uintptr_t)g->errno_slot);
    guest_free(g,(void *)(uintptr_t)g->strerror_buffer);guest_free(g,(void *)(uintptr_t)g->setlocale_buffer);
    guest_free(g,(void *)(uintptr_t)g->localeconv_buffer);
    if(g->default_locale)guest_freelocale(g,g->default_locale);
    __atomic_sub_fetch(&shared->active_threads, 1, __ATOMIC_RELEASE);
    pthread_cond_destroy(&thread->cond);pthread_mutex_destroy(&thread->lock);
    free(thread);
    return (void *)(uintptr_t)result;
}

typedef struct { Guest *guest; uint64_t compare; int failed; } SortContext;
static int guest_compare(void *opaque, const void *left, const void *right) {
    SortContext *ctx = opaque;
    uint64_t args[8] = {(uintptr_t)left, (uintptr_t)right}, result = 0;
    if (guest_callback(ctx->guest, ctx->compare, args, &result)) { ctx->failed = 1; return 0; }
    return (int32_t)result;
}

typedef struct Futex {
    uint64_t address;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    unsigned waiters;
    struct Futex *next;
} Futex;
static pthread_mutex_t futex_table_lock = PTHREAD_MUTEX_INITIALIZER;
static Futex *futexes;

static Futex *get_futex(uint64_t address) {
    pthread_mutex_lock(&futex_table_lock);
    Futex *f = futexes;
    while (f && f->address != address) f = f->next;
    if (!f) {
        f = calloc(1, sizeof(*f));
        if (f) {
            f->address = address;
            pthread_mutex_init(&f->mutex, NULL);
            pthread_cond_init(&f->condition, NULL);
            f->next = futexes; futexes = f;
        }
    }
    pthread_mutex_unlock(&futex_table_lock);
    return f;
}

static uint64_t guest_futex(Guest *g, const uint64_t a[8], int *failed) {
    Futex *f = get_futex(a[1]);
    if (!f) { *(int *)(uintptr_t)g->errno_slot = 12; return UINT64_MAX; }
    unsigned op = (unsigned)a[2] & 0x7f;
    pthread_mutex_lock(&f->mutex);
    if (op == 0) { // FUTEX_WAIT[_PRIVATE]
        if (__atomic_load_n((uint32_t *)(uintptr_t)a[1], __ATOMIC_SEQ_CST) != (uint32_t)a[3]) {
            pthread_mutex_unlock(&f->mutex);
            *(int *)(uintptr_t)g->errno_slot = 11; return UINT64_MAX; // EAGAIN
        }
        ++f->waiters;
        int error;
        if (a[4]) {
            const struct timespec *relative = (const struct timespec *)(uintptr_t)a[4];
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += relative->tv_sec;
            deadline.tv_nsec += relative->tv_nsec;
            if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
            error = pthread_cond_timedwait(&f->condition, &f->mutex, &deadline);
        } else error = pthread_cond_wait(&f->condition, &f->mutex);
        --f->waiters;
        pthread_mutex_unlock(&f->mutex);
        if (error) { *(int *)(uintptr_t)g->errno_slot = linux_errno(error); return UINT64_MAX; }
        return 0;
    }
    if (op == 1) { // FUTEX_WAKE[_PRIVATE]
        unsigned count = f->waiters < a[3] ? f->waiters : (unsigned)a[3];
        if (count == f->waiters) pthread_cond_broadcast(&f->condition);
        else for (unsigned i = 0; i < count; ++i) pthread_cond_signal(&f->condition);
        pthread_mutex_unlock(&f->mutex);
        return count;
    }
    pthread_mutex_unlock(&f->mutex);
    fprintf(stderr, "Unimplemented Linux futex operation %#x\n", (unsigned)a[2]);
    *failed = 1; return 0;
}

static void remember_speaker(Speaker *speakers,size_t *count,const char *name,size_t n,const char *gender,size_t m) {
    if(!n || n>=sizeof speakers[0].name || m>=sizeof speakers[0].gender || *count>=128)return;
    for(size_t i=0;i<*count;++i)if(strlen(speakers[i].name)==n && !strncmp(speakers[i].name,name,n))return;
    Speaker *entry=&speakers[(*count)++];memcpy(entry->name,name,n);entry->name[n]=0;
    if(m)memcpy(entry->gender,gender,m);entry->gender[m]=0;
}
static uint64_t invoke(Guest *g, const char *name, const uint64_t a[8], int *failed) {
    if (!strcmp(name, "__guest_logger")) {
        const char *message = (const char *)(uintptr_t)a[1];
        const char *p = message;
        while (p && (p = strstr(p, "speaker: \""))) {
            const char *start = p + strlen("speaker: \"");
            const char *end = strchr(start, '"');
            if(!end)break;size_t n=(size_t)(end-start);
            const char *q=end+1,*gender_start=NULL,*gender_end=NULL;while(isspace((unsigned char)*q))++q;
            if(q>end+1 && !strncmp(q,"gender: \"",9)){gender_start=q+9;gender_end=strchr(gender_start,'"');}
            size_t m=gender_end?(size_t)(gender_end-gender_start):0;
            Guest *shared = g->master;
            pthread_mutex_lock(&shared->once_lock);
            remember_speaker(shared->fallback_speakers,&shared->fallback_speaker_count,start,n,NULL,0);
            if(m)remember_speaker(shared->speakers,&shared->speaker_count,start,n,gender_start,m);
            pthread_mutex_unlock(&shared->once_lock);
            p=end+1;
        }
        if (g->verbose || g->trace)
            fprintf(stderr, "[engine %d] %s\n", (int)a[0], message);
        return 0;
    }
    if (!strcmp(name, "srand") || !strcmp(name, "rand")) {
        pthread_mutex_lock(&random_lock);
        if (!strcmp(name, "srand")) { linux_srand((uint32_t)a[0]); pthread_mutex_unlock(&random_lock); return 0; }
        if (!random_initialized) linux_srand(1);
        uint32_t value = linux_rand_next();
        pthread_mutex_unlock(&random_lock);
        return value;
    }
    if (!strcmp(name, "fwrite") && !g->verbose && a[3] == (uintptr_t)stderr &&
        a[1] && a[2] <= SIZE_MAX / a[1] && a[1] * a[2] >= 5 &&
        !memcmp((const void *)(uintptr_t)a[0], "I0000", 5)) return a[2];
    if (!strcmp(name, "malloc")) return (uintptr_t)guest_malloc(g, (size_t)a[0]);
    if (!strcmp(name, "aligned_alloc") || !strcmp(name, "memalign")) {
        if (!a[0] || (a[0] & (a[0] - 1))) return 0;
        return (uintptr_t)guest_aligned_malloc(g, (size_t)a[0], (size_t)a[1]);
    }
    if (!strcmp(name, "posix_memalign")) {
        if (a[1] < sizeof(void *) || (a[1] & (a[1] - 1))) return EINVAL;
        void *p = guest_aligned_malloc(g, (size_t)a[1], (size_t)a[2]);
        if (!p) return ENOMEM;
        *(uintptr_t *)(uintptr_t)a[0] = (uintptr_t)p;
        return 0;
    }
    if (!strcmp(name, "calloc")) {
        if (a[1] && a[0] > SIZE_MAX / a[1]) return 0;
        size_t size = (size_t)(a[0] * a[1]);
        void *p = guest_malloc(g, size);
        if (p) memset(p, 0, size);
        return (uintptr_t)p;
    }
    if (!strcmp(name, "realloc")) {
        if (!a[0]) return (uintptr_t)guest_malloc(g, (size_t)a[1]);
        if (!a[1]) { guest_free(g, (void *)(uintptr_t)a[0]); return 0; }
        uint64_t *old = (uint64_t *)(uintptr_t)a[0] - 2;
        if (old[1] != UINT64_C(0x4754545348454150)) {
            fprintf(stderr, "realloc of non-guest allocation: %#llx\n", (unsigned long long)a[0]);
            *failed = 1; return 0;
        }
        if (old[0] >= a[1]) return a[0];
        void *p = guest_malloc(g, (size_t)a[1]);
        if (p) memcpy(p, (void *)(uintptr_t)a[0], old[0] < a[1] ? old[0] : (size_t)a[1]);
        if (p) guest_free(g, (void *)(uintptr_t)a[0]);
        return (uintptr_t)p;
    }
    if (!strcmp(name, "free")) { guest_free(g, (void *)(uintptr_t)a[0]); return 0; }
    if (!strcmp(name, "strdup")) {
        const char *s = (const char *)(uintptr_t)a[0];
        size_t length = strlen(s) + 1;
        void *p = guest_malloc(g, length);
        if (p) memcpy(p, s, length);
        return (uintptr_t)p;
    }
    if (!strcmp(name, "getenv")) {
        const char *name=(const char *)(uintptr_t)a[0];if(!name || !*name || strchr(name,'='))return 0;
        char **environment=*(char ***)(uintptr_t)g->master->environ_slot;size_t length=strlen(name);
        for(size_t i=0;environment && environment[i];++i)
            if(!strncmp(environment[i],name,length) && environment[i][length]=='=')return (uintptr_t)(environment[i]+length+1);
        return 0;
    }
    if (!strcmp(name, "__errno_location")) return g->errno_slot;
    if (!strcmp(name, "clock_gettime")) {
        clockid_t clock_id;
        switch (a[0]) {
        case 0: case 5: clock_id = CLOCK_REALTIME; break;
        case 1: case 6: case 7: clock_id = CLOCK_MONOTONIC; break;
        case 2: clock_id = CLOCK_PROCESS_CPUTIME_ID; break;
        case 3: clock_id = CLOCK_THREAD_CPUTIME_ID; break;
        case 4: clock_id = CLOCK_MONOTONIC_RAW; break;
        default: *(int *)(uintptr_t)g->errno_slot = 22; return UINT64_MAX;
        }
        int r = clock_gettime(clock_id, (struct timespec *)(uintptr_t)a[1]);
        if (r) *(int *)(uintptr_t)g->errno_slot = linux_errno(errno);
        return (uint64_t)(int64_t)r;
    }
    if (!strcmp(name, "gettimeofday")) {
        struct timeval tv;int r=gettimeofday(&tv,(struct timezone *)(uintptr_t)a[1]);
        if(r){*(int *)(uintptr_t)g->errno_slot=linux_errno(errno);return UINT64_MAX;}
        if(a[0]){int64_t *out=(int64_t *)(uintptr_t)a[0];out[0]=(int64_t)tv.tv_sec;out[1]=(int64_t)tv.tv_usec;}
        return 0;
    }
    if (!strcmp(name, "strerror_r")) {
        char *buffer = (char *)(uintptr_t)a[1];
        if (strerror_r(host_errno((int)a[0]), buffer, (size_t)a[2]) && a[2]) buffer[0] = '\0';
        return a[1]; // glibc's GNU strerror_r returns char *.
    }
    if (!strcmp(name, "strerror")) {
        const char *s = strerror(host_errno((int)a[0]));
        if(!g->strerror_buffer)g->strerror_buffer=(uintptr_t)guest_malloc(g,256);
        if(g->strerror_buffer)strlcpy((char *)(uintptr_t)g->strerror_buffer,s,256);
        return g->strerror_buffer;
    }
    if (!strcmp(name, "setlocale")) {
        static const int categories[] = {LC_CTYPE, LC_NUMERIC, LC_TIME, LC_COLLATE, LC_MONETARY, LC_MESSAGES, LC_ALL};
        if (a[0] >= sizeof(categories) / sizeof(categories[0])) return 0;
        const char *value = setlocale(categories[a[0]], (const char *)(uintptr_t)a[1]);
        if (!value) return 0;
        size_t length = strlen(value) + 1;
        guest_free(g,(void *)(uintptr_t)g->setlocale_buffer);
        char *copy = guest_malloc(g, length);g->setlocale_buffer=(uintptr_t)copy;
        if (copy) memcpy(copy, value, length);
        return (uintptr_t)copy;
    }
    if (!strcmp(name,"localeconv")) {
        struct lconv *host=localeconv();
        const char *strings[]={host->decimal_point,host->thousands_sep,host->grouping,host->int_curr_symbol,
            host->currency_symbol,host->mon_decimal_point,host->mon_thousands_sep,host->mon_grouping,host->positive_sign,host->negative_sign};
        size_t size=96;for(size_t i=0;i<10;++i)size+=strlen(strings[i]?strings[i]:"")+1;
        uint8_t *out=guest_malloc(g,size);if(!out){*(int *)(uintptr_t)g->errno_slot=12;return 0;}memset(out,0,size);
        char *at=(char *)out+96;
        for(size_t i=0;i<10;++i){const char *s=strings[i]?strings[i]:"";size_t n=strlen(s)+1;
            ((uint64_t *)out)[i]=(uintptr_t)at;memcpy(at,s,n);at+=n;}
        char attrs[]={host->int_frac_digits,host->frac_digits,host->p_cs_precedes,host->p_sep_by_space,host->n_cs_precedes,
            host->n_sep_by_space,host->p_sign_posn,host->n_sign_posn,host->int_p_cs_precedes,host->int_p_sep_by_space,
            host->int_n_cs_precedes,host->int_n_sep_by_space,host->int_p_sign_posn,host->int_n_sign_posn};
        memcpy(out+80,attrs,sizeof attrs);guest_free(g,(void *)(uintptr_t)g->localeconv_buffer);g->localeconv_buffer=(uintptr_t)out;return (uintptr_t)out;
    }
    if (!strcmp(name, "getauxval") || !strcmp(name, "__getauxval"))
        return a[0] == 16 ? 3 : 0; // AArch64 baseline FP and SIMD only.
    if (!strcmp(name, "syscall")) {
        if (a[0] == 178) return g->tid;
        if (a[0] == 98) return guest_futex(g, a, failed);
        if (a[0] == 222) {
            if (g->trace) fprintf(stderr, "  mmap addr=%#llx len=%#llx prot=%#llx flags=%#llx fd=%lld off=%#llx\n",
                                  (unsigned long long)a[1], (unsigned long long)a[2], (unsigned long long)a[3],
                                  (unsigned long long)a[4], (long long)a[5], (unsigned long long)a[6]);
            return guest_mmap(g, a + 1);
        }
        if (a[0] == 215) return guest_munmap(g,a[1],a[2]);
        if (a[0] == 226 && g->native_mode && (a[3] & 4)) { *(int *)(uintptr_t)g->errno_slot=95; return UINT64_MAX; }
        if (a[0] == 226 || a[0] == 233) return 0; // mprotect, madvise
        fprintf(stderr, "Unimplemented Linux syscall %llu\n", (unsigned long long)a[0]);
        *failed = 1; return 0;
    }
    if (!strcmp(name, "mmap")) return guest_mmap(g, a);
    if (!strcmp(name, "munmap")) return guest_munmap(g,a[0],a[1]);
    if (!strcmp(name, "mprotect") && g->native_mode && (a[2] & 4)) { *(int *)(uintptr_t)g->errno_slot=95; return UINT64_MAX; }
    if (!strcmp(name, "mprotect") || !strcmp(name, "madvise")) return 0;
    if (!strcmp(name, "sysconf") || !strcmp(name, "__sysconf")) {
        if (a[0] == 30) return 16384; // _SC_PAGESIZE
        if (a[0] == 83 || a[0] == 84) return 1; // configured/online CPUs
        if (a[0] == 2) return 100; // clock ticks
        *(int *)(uintptr_t)g->errno_slot = EINVAL;
        return UINT64_MAX;
    }
    if (!strcmp(name, "open") || !strcmp(name, "open64") || !strcmp(name, "__open64_2")) {
        const char *path = (const char *)(uintptr_t)a[0];
        if(a[1]&0x10000){*(int *)(uintptr_t)g->errno_slot=95;return UINT64_MAX;} // Linux O_DIRECT has no equivalent here.
        int flags = linux_open_flags(a[1]);
        if (g->trace) fprintf(stderr, "  open %s Linux flags=%#llx\n", path, (unsigned long long)a[1]);
        if (!(flags & (O_WRONLY | O_RDWR))) {
            int synthetic = virtual_cpu_file(path);
            if (synthetic != -2) return (uint64_t)(int64_t)synthetic;
        }
        if ((flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) && !write_path_allowed(path)) {
            *(int *)(uintptr_t)g->errno_slot = EACCES; return UINT64_MAX;
        }
        int fd = open(path, flags, (mode_t)a[2]);
        *(int *)(uintptr_t)g->errno_slot = linux_errno(errno);
        return (uint64_t)(int64_t)fd;
    }
    if (!strcmp(name, "fopen") || !strcmp(name, "fopen64")) {
        const char *path = (const char *)(uintptr_t)a[0], *mode = (const char *)(uintptr_t)a[1];
        if (g->trace) fprintf(stderr, "  fopen %s mode=%s\n", path, mode);
        if (strpbrk(mode, "wa+") && !write_path_allowed(path)) {
            *(int *)(uintptr_t)g->errno_slot = EACCES; return 0;
        }
        FILE *f = fopen(path, mode);
        *(int *)(uintptr_t)g->errno_slot = linux_errno(errno);
        return (uintptr_t)f;
    }
    if (!strcmp(name, "sched_getcpu")) return 0;
    if (!strcmp(name, "tzset")) {guest_update_timezone(g);return 0;}
    if (!strcmp(name,"getrlimit") || !strcmp(name,"getrlimit64")) {
        static const int resources[]={RLIMIT_CPU,RLIMIT_FSIZE,RLIMIT_DATA,RLIMIT_STACK,RLIMIT_CORE,RLIMIT_RSS,RLIMIT_NPROC,RLIMIT_NOFILE,RLIMIT_MEMLOCK,RLIMIT_AS};
        if(a[0]>=sizeof resources/sizeof resources[0]){*(int *)(uintptr_t)g->errno_slot=22;return UINT64_MAX;}
        struct rlimit limits;if(getrlimit(resources[a[0]],&limits)){*(int *)(uintptr_t)g->errno_slot=linux_errno(errno);return UINT64_MAX;}
        uint64_t *out=(uint64_t *)(uintptr_t)a[1];out[0]=limits.rlim_cur==RLIM_INFINITY?UINT64_MAX:limits.rlim_cur;
        out[1]=limits.rlim_max==RLIM_INFINITY?UINT64_MAX:limits.rlim_max;return 0;
    }
    if (!strcmp(name,"getcwd") || !strcmp(name,"realpath") || !strcmp(name,"__realpath_chk")) {
        errno=host_errno(*(int *)(uintptr_t)g->errno_slot);char *value=NULL;
        if(!strcmp(name,"getcwd"))value=getcwd((char *)(uintptr_t)a[0],(size_t)a[1]);
        else if(!strcmp(name,"__realpath_chk")) {
            char *resolved=realpath((const char *)(uintptr_t)a[0],NULL);
            if(resolved && strlen(resolved)+1>a[2]){free(resolved);fprintf(stderr,"Guest realpath buffer overflow\n");*failed=1;return 0;}
            if(resolved){if(a[1]){strcpy((char *)(uintptr_t)a[1],resolved);free(resolved);value=(char *)(uintptr_t)a[1];}else value=resolved;}
        } else value=realpath((const char *)(uintptr_t)a[0],(char *)(uintptr_t)a[1]);
        *(int *)(uintptr_t)g->errno_slot=linux_errno(errno);
        int allocated=!strcmp(name,"getcwd")?!a[0]:!a[1];
        if(value && allocated){size_t length=strlen(value)+1;char *copy=guest_malloc(g,length);
            if(copy)memcpy(copy,value,length);free(value);if(!copy)*(int *)(uintptr_t)g->errno_slot=12;return (uintptr_t)copy;}
        return (uintptr_t)value;
    }
    if (!strcmp(name,"pread64") || !strcmp(name,"__pread64_chk") || !strcmp(name,"__read_chk") || !strcmp(name,"__readlink_chk") ||
        !strcmp(name,"__fread_chk") || !strcmp(name,"__fgets_chk")) {
        errno=host_errno(*(int *)(uintptr_t)g->errno_slot);uint64_t result=0;int overflow=0;
        if(!strcmp(name,"__read_chk")) {overflow=a[2]>a[3];if(!overflow)result=(uint64_t)(int64_t)read((int)a[0],(void *)(uintptr_t)a[1],(size_t)a[2]);}
        else if(!strcmp(name,"__readlink_chk")){overflow=a[2]>a[3];if(!overflow)result=(uint64_t)(int64_t)readlink((const char *)(uintptr_t)a[0],(char *)(uintptr_t)a[1],(size_t)a[2]);}
        else if(!strcmp(name,"pread64") || !strcmp(name,"__pread64_chk")) {
            overflow=!strcmp(name,"__pread64_chk") && a[2]>a[4];
            if(!overflow)result=(uint64_t)(int64_t)pread((int)a[0],(void *)(uintptr_t)a[1],(size_t)a[2],(off_t)a[3]);
        } else if(!strcmp(name,"__fread_chk")) {
            overflow=a[2] && (a[3]>SIZE_MAX/a[2] || a[2]*a[3]>a[1]);
            if(!overflow)result=fread((void *)(uintptr_t)a[0],(size_t)a[2],(size_t)a[3],(FILE *)(uintptr_t)a[4]);
        } else {overflow=(int32_t)a[2]>0 && (uint32_t)a[2]>a[1];
            if(!overflow)result=(uintptr_t)fgets((char *)(uintptr_t)a[0],(int)a[2],(FILE *)(uintptr_t)a[3]);}
        if(overflow){fprintf(stderr,"Guest fortified read overflow: %s\n",name);*failed=1;return 0;}
        *(int *)(uintptr_t)g->errno_slot=linux_errno(errno);return result;
    }
    if (!strcmp(name,"sem_init") || !strcmp(name,"sem_wait") || !strcmp(name,"sem_trywait") ||
        !strcmp(name,"sem_timedwait") || !strcmp(name,"sem_post") || !strcmp(name,"sem_getvalue") || !strcmp(name,"sem_destroy"))
        return guest_semaphore(g,name,a);
    if(!strcmp(name,"poll")) {
        if(a[1]>4096){*(int *)(uintptr_t)g->errno_slot=22;return UINT64_MAX;}
        struct pollfd *guest=(struct pollfd *)(uintptr_t)a[0];nfds_t count=(nfds_t)a[1];
        struct pollfd *host=calloc(count?count:1,sizeof *host);if(!host){*(int *)(uintptr_t)g->errno_slot=12;return UINT64_MAX;}
        for(nfds_t i=0;i<count;++i){short events=guest[i].events;host[i].fd=guest[i].fd;host[i].events=events&0x00ff;
            if(events&0x0100)host[i].events|=POLLOUT;if(events&0x0200)host[i].events|=POLLWRBAND;}
        int result=poll(host,count,(int)a[2]);if(result<0)*(int *)(uintptr_t)g->errno_slot=linux_errno(errno);
        else for(nfds_t i=0;i<count;++i){short events=guest[i].events;
            guest[i].revents=(host[i].revents&(POLLERR|POLLHUP|POLLNVAL)) | (host[i].revents&events&0x00c7);
            if((host[i].revents&POLLOUT) && (events&0x0100))guest[i].revents|=0x0100;
            if(host[i].revents&POLLWRBAND)guest[i].revents|=0x0200;}
        free(host);return (uint64_t)(int64_t)result;
    }
    if (!strcmp(name, "stat") || !strcmp(name, "stat64") || !strcmp(name, "fstat") ||
        !strcmp(name, "fstat64") || !strcmp(name, "lstat") || !strcmp(name, "lstat64"))
        return guest_stat(g, name, a);
    if (!strcmp(name, "prctl")) { *(int *)(uintptr_t)g->errno_slot = 38; return UINT64_MAX; }
    if (g->native_mode && !strcmp(name, "sigaction")) {
        /* glibc aarch64: handler, 1024-bit mask, flags/padding, restorer. */
        enum { ACTION_SIZE = 152, SIGNAL_COUNT = 65 };
        unsigned signal = (unsigned)a[0];
        if (!signal || signal >= SIGNAL_COUNT || (a[1] && (signal == 9 || signal == 19))) {
            *(int *)(uintptr_t)g->errno_slot = 22; return UINT64_MAX;
        }
        unsigned char incoming[ACTION_SIZE];
        if (a[1]) memcpy(incoming, (void *)(uintptr_t)a[1], sizeof incoming);
        Guest *shared = g->master;
        pthread_mutex_lock(&shared->once_lock);
        if (!shared->native_sigactions) shared->native_sigactions = calloc(SIGNAL_COUNT, ACTION_SIZE);
        if (!shared->native_sigactions) {
            pthread_mutex_unlock(&shared->once_lock);
            *(int *)(uintptr_t)g->errno_slot = 12; return UINT64_MAX;
        }
        unsigned char *action = (unsigned char *)shared->native_sigactions + signal * ACTION_SIZE;
        if (a[2]) memcpy((void *)(uintptr_t)a[2], action, ACTION_SIZE);
        if (a[1]) memcpy(action, incoming, ACTION_SIZE);
        pthread_mutex_unlock(&shared->once_lock);
        return 0;
    }
    if (g->native_mode && !strcmp(name, "sigfillset")) {
        memset((void *)(uintptr_t)a[0], 0xff, 128); return 0;
    }
    if (g->native_mode && !strcmp(name, "pthread_sigmask")) {
        uint64_t newmask[16];
        if (a[1]) memcpy(newmask, (void *)(uintptr_t)a[1], sizeof newmask);
        if (a[2]) memcpy((void *)(uintptr_t)a[2], g->native_guest_sigmask, sizeof newmask);
        if (a[1]) {
            if (a[0] > 2) return 22;
            for (unsigned i = 0; i < 16; ++i) {
                if (a[0] == 0) g->native_guest_sigmask[i] |= newmask[i];
                else if (a[0] == 1) g->native_guest_sigmask[i] &= ~newmask[i];
                else g->native_guest_sigmask[i] = newmask[i];
            }
        }
        return 0;
    }
    if (!strcmp(name, "sigfillset") || !strcmp(name, "pthread_sigmask")) return 0;
    if (!strcmp(name, "pthread_key_create")) {
        pthread_key_t key;
        int ret = pthread_key_create(&key, NULL); // Guest destructors need a guest-thread exit bridge.
        if (!ret) *(uint32_t *)(uintptr_t)a[0] = (uint32_t)key;
        return linux_errno(ret);
    }
    if (!strcmp(name, "pthread_once")) {
        uint32_t *state = (uint32_t *)(uintptr_t)a[0];
        Guest *shared = g->master;
        pthread_mutex_lock(&shared->once_lock);
        while (__atomic_load_n(state, __ATOMIC_ACQUIRE) == 1)
            pthread_cond_wait(&shared->once_cond, &shared->once_lock);
        if (__atomic_load_n(state, __ATOMIC_ACQUIRE) == 2) {
            pthread_mutex_unlock(&shared->once_lock); return 0;
        }
        __atomic_store_n(state, 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&shared->once_lock);
        uint64_t ignored;
        int status = guest_callback(g, a[1], NULL, &ignored);
        pthread_mutex_lock(&shared->once_lock);
        __atomic_store_n(state, status ? 0 : 2, __ATOMIC_RELEASE);
        pthread_cond_broadcast(&shared->once_cond);
        pthread_mutex_unlock(&shared->once_lock);
        if (status) *failed = 1;
        return 0;
    }
    if (!strcmp(name, "pthread_create")) {
        GuestThread *thread = calloc(1, sizeof(*thread));
        if (!thread) return ENOMEM;
        guest_thread_clone(&thread->guest, g);
        thread->guest.tid = __atomic_add_fetch(&g->master->next_tid, 1, __ATOMIC_RELAXED);
        thread->entry = a[2]; thread->argument = a[3];
        pthread_mutex_init(&thread->lock,NULL);pthread_cond_init(&thread->cond,NULL);pthread_mutex_lock(&thread->lock);
        pthread_t handle;
        __atomic_add_fetch(&g->master->active_threads, 1, __ATOMIC_RELEASE);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        if (g->native_mode) pthread_attr_setstacksize(&attr, 8u * 1024u * 1024u);
        int error = pthread_create(&handle, &attr, guest_thread_main, thread);
        pthread_attr_destroy(&attr);
        if (error) {
            __atomic_sub_fetch(&g->master->active_threads, 1, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&thread->lock);pthread_cond_destroy(&thread->cond);pthread_mutex_destroy(&thread->lock);
            free(thread); return linux_errno(error);
        }
        while(!thread->ready)pthread_cond_wait(&thread->cond,&thread->lock);
        error=thread->error;thread->accepted=1;pthread_cond_signal(&thread->cond);pthread_mutex_unlock(&thread->lock);
        if(error){pthread_join(handle,NULL);return 11;}
        *(uint64_t *)(uintptr_t)a[0] = (uintptr_t)handle;
        return 0;
    }
    if (!strcmp(name, "qsort")) {
        SortContext ctx = {g, a[3], 0};
        qsort_r((void *)(uintptr_t)a[0], (size_t)a[1], (size_t)a[2], &ctx, guest_compare);
        if (ctx.failed) *failed = 1;
        return 0;
    }
    if (!strcmp(name, "dlsym")) {
        const char *symbol = (const char *)(uintptr_t)a[1];
        if (g->trace) fprintf(stderr, "  guest dlsym %s\n", symbol);
        return guest_symbol(g, symbol);
    }
    if (!strcmp(name, "_dl_find_object")) {
        // glibc's aarch64 dl_find_object has five pointer-sized fields and seven reserved words.
        uint64_t address=a[0],*out=(uint64_t *)(uintptr_t)a[1];
        for(size_t i=0;i<g->object_count;++i){ElfObject *obj=&g->objects[i];uint64_t base=(uintptr_t)obj->base;
            if(address>=base && address-base<obj->size){memset(out,0,12*sizeof *out);out[1]=base;out[2]=base+obj->size;
                out[3]=obj->link_map;out[4]=obj->eh_frame;return 0;}}
        return UINT64_MAX;
    }
    if (!strcmp(name, "dl_iterate_phdr")) {
        typedef struct {uint64_t addr,name,phdr;uint16_t phnum;uint8_t pad[6];uint64_t adds,subs,tls_modid,tls_data;} PhdrInfo;
        PhdrInfo *info=guest_malloc(g,sizeof *info);if(!info){*failed=1;return 0;}
        uint64_t result=0;
        for(size_t i=0;i<g->object_count;++i){ElfObject *obj=&g->objects[i];
            *info=(PhdrInfo){(uintptr_t)obj->base,obj->guest_path,obj->phdr,(uint16_t)obj->phnum,{0},g->object_count,0,
                             obj->tls_memsz?i+1:0,obj->tls_memsz?(uintptr_t)(g->tls+obj->tls_offset):0};
            uint64_t args[8]={(uintptr_t)info,sizeof *info,a[1]};
            if(guest_callback(g,a[0],args,&result)){*failed=1;break;}if((int32_t)result)break;
        }
        guest_free(g,info);return result;
    }
    static const char *const unsupported[] = {
        "openat", "unlinkat", "unlink", "remove", "rename", "link", "symlink", "chdir", "mkdir", "rmdir",
        "truncate", "ftruncate", "ftruncate64", "fallocate64", "fchmod", "fchmodat", "utimensat",
        "statvfs", "readdir", "readdir64", "fdopendir",
        "dlopen", "sigaction", "sigaltstack", "pthread_kill", "kill",
        "setrlimit64", "setrlimit", "pthread_setname_np",
        "pthread_attr_init", "pthread_attr_destroy", "pthread_attr_setscope", "pthread_attr_setdetachstate",
        "pthread_attr_setstacksize", "pthread_attr_setguardsize", "pthread_attr_setschedpolicy",
        "pthread_attr_setinheritsched", "pthread_attr_setschedparam", "pthread_attr_getschedparam",
        "__pthread_register_cancel", "__pthread_unregister_cancel", "__pthread_unwind_next", "__sigsetjmp", NULL
    };
    if (in_names(name, unsupported)) {
        fprintf(stderr, "Unsupported Linux import: %s\n", name);
        *failed = 1; return 0;
    }
    if (!strcmp(name, "ftello64")) name = "ftello";
    else if (!strcmp(name, "fseeko64")) name = "fseeko";
    else if (!strcmp(name, "lseek64")) name = "lseek";
    if (!strcmp(name, "newlocale")) return guest_newlocale(g, (const char *)(uintptr_t)a[1]);
    if (!strcmp(name, "freelocale")) {guest_freelocale(g,a[0]);return 0;}
    if (!strcmp(name, "uselocale")) {
        uint64_t previous = g->current_locale;
        if (a[0]) {
            locale_t host = a[0] == UINT64_MAX ? LC_GLOBAL_LOCALE : host_locale(a[0]);
            if (!uselocale(host)) return 0;
            g->current_locale = a[0];
        }
        return previous;
    }
    if (!strcmp(name, "__ctype_get_mb_cur_max")) return MB_CUR_MAX;
    if (!strcmp(name, "__ctype_b_loc") || !strcmp(name, "__ctype_tolower_loc") ||
        !strcmp(name, "__ctype_toupper_loc")) {
        if (!g->default_locale) g->default_locale = guest_newlocale(g, "C");
        if (!g->default_locale) return 0;
        return g->default_locale + (!strcmp(name, "__ctype_b_loc") ? 0x68 :
                                    !strcmp(name, "__ctype_tolower_loc") ? 0x70 : 0x78);
    }
    if (!strcmp(name, "pthread_mutexattr_init")) { *(int *)(uintptr_t)a[0] = 0; return 0; }
    if (!strcmp(name, "pthread_mutexattr_destroy")) return 0;
    if (!strcmp(name, "pthread_mutexattr_settype")) { *(int *)(uintptr_t)a[0] = (int)a[1]; return 0; }
    if (!strcmp(name, "pthread_mutex_init")) {
        *(uintptr_t *)(uintptr_t)a[0] = 0;
        int kind = a[1] ? *(int *)(uintptr_t)a[1] : 0;
        return guest_mutex(a[0], kind) ? 0 : ENOMEM;
    }
    if (!strcmp(name, "pthread_mutex_lock") || !strcmp(name, "pthread_mutex_trylock") ||
        !strcmp(name, "pthread_mutex_unlock") || !strcmp(name, "pthread_mutex_destroy")) {
        pthread_mutex_t *mutex = guest_mutex(a[0], 0);
        if (!mutex) return EINVAL;
        if (!strcmp(name, "pthread_mutex_lock")) return linux_errno(pthread_mutex_lock(mutex));
        if (!strcmp(name, "pthread_mutex_trylock")) return linux_errno(pthread_mutex_trylock(mutex));
        if (!strcmp(name, "pthread_mutex_unlock")) return linux_errno(pthread_mutex_unlock(mutex));
        int result = pthread_mutex_destroy(mutex);
        if (!result) { *(uintptr_t *)(uintptr_t)a[0] = 0; free(mutex); }
        return linux_errno(result);
    }
    if (!strcmp(name, "pthread_cond_init")) {
        *(uintptr_t *)(uintptr_t)a[0] = 0;
        return guest_cond(a[0]) ? 0 : ENOMEM;
    }
    if(!strcmp(name,"pthread_rwlock_init") || !strcmp(name,"pthread_rwlock_rdlock") || !strcmp(name,"pthread_rwlock_wrlock") ||
       !strcmp(name,"pthread_rwlock_unlock") || !strcmp(name,"pthread_rwlock_destroy")) {
        if(!a[0])return 22;
        if(!strcmp(name,"pthread_rwlock_init")){if(a[1])return 95;*(uintptr_t *)(uintptr_t)a[0]=0;return guest_rwlock(a[0])?0:12;}
        uintptr_t *slot=(uintptr_t *)(uintptr_t)a[0];
        if(!strcmp(name,"pthread_rwlock_destroy") && !__atomic_load_n(slot,__ATOMIC_ACQUIRE))return 0;
        pthread_rwlock_t *lock=guest_rwlock(a[0]);if(!lock)return 12;int result;
        if(!strcmp(name,"pthread_rwlock_rdlock"))result=pthread_rwlock_rdlock(lock);
        else if(!strcmp(name,"pthread_rwlock_wrlock"))result=pthread_rwlock_wrlock(lock);
        else if(!strcmp(name,"pthread_rwlock_unlock"))result=pthread_rwlock_unlock(lock);
        else {result=pthread_rwlock_destroy(lock);if(!result){*slot=0;free(lock);}}
        return linux_errno(result);
    }
    if(!strcmp(name,"pthread_getschedparam") || !strcmp(name,"sched_get_priority_max") || !strcmp(name,"sched_get_priority_min")) {
        if(!strcmp(name,"pthread_getschedparam")) {
            int policy=0;struct sched_param param={0};int result=pthread_getschedparam((pthread_t)(uintptr_t)a[0],&policy,&param);
            if(!result){*(int *)(uintptr_t)a[1]=policy==SCHED_FIFO?1:policy==SCHED_RR?2:0;*(int *)(uintptr_t)a[2]=param.sched_priority;}
            return linux_errno(result);
        }
        int policy=a[0]==0?SCHED_OTHER:a[0]==1?SCHED_FIFO:a[0]==2?SCHED_RR:-1;
        if(policy<0){*(int *)(uintptr_t)g->errno_slot=22;return UINT64_MAX;}
        int result=!strcmp(name,"sched_get_priority_max")?sched_get_priority_max(policy):sched_get_priority_min(policy);
        if(result<0)*(int *)(uintptr_t)g->errno_slot=linux_errno(errno);return (uint64_t)(int64_t)result;
    }
    if (!strcmp(name, "pthread_cond_broadcast") || !strcmp(name, "pthread_cond_signal") ||
        !strcmp(name, "pthread_cond_wait") || !strcmp(name, "pthread_cond_timedwait") ||
        !strcmp(name, "pthread_cond_destroy")) {
        pthread_cond_t *condition = guest_cond(a[0]);
        if (!condition) return EINVAL;
        if (!strcmp(name, "pthread_cond_broadcast")) return linux_errno(pthread_cond_broadcast(condition));
        if (!strcmp(name, "pthread_cond_signal")) return linux_errno(pthread_cond_signal(condition));
        if (!strcmp(name, "pthread_cond_wait") || !strcmp(name, "pthread_cond_timedwait")) {
            pthread_mutex_t *mutex = guest_mutex(a[1], 0);
            if (!mutex) return EINVAL;
            if (!strcmp(name, "pthread_cond_wait")) return linux_errno(pthread_cond_wait(condition, mutex));
            return linux_errno(pthread_cond_timedwait(condition, mutex, (const struct timespec *)(uintptr_t)a[2]));
        }
        int result = pthread_cond_destroy(condition);
        if (!result) { *(uintptr_t *)(uintptr_t)a[0] = 0; free(condition); }
        return linux_errno(result);
    }
    if (!strcmp(name, "strtoll_l") || !strcmp(name, "__isoc23_strtoll_l") ||
        !strcmp(name, "strtoull_l") || !strcmp(name, "__isoc23_strtoull_l")) {
        errno = host_errno(*(int *)(uintptr_t)g->errno_slot);
        uint64_t result = strstr(name, "strtoull") ?
            strtoull_l((const char *)(uintptr_t)a[0], (char **)(uintptr_t)a[1], (int)a[2], host_locale(a[3])) :
            (uint64_t)strtoll_l((const char *)(uintptr_t)a[0], (char **)(uintptr_t)a[1], (int)a[2], host_locale(a[3]));
        *(int *)(uintptr_t)g->errno_slot = linux_errno(errno); return result;
    }
    if(!strcmp(name,"__cxa_thread_atexit_impl")) {
        ThreadDestructor *d=malloc(sizeof *d);if(!d)return UINT64_MAX;
        *d=(ThreadDestructor){a[0],a[1],g->thread_destructors};g->thread_destructors=d;return 0;
    }
    if (!strcmp(name, "__cxa_atexit") || !strcmp(name, "atexit")) {
        if (!a[0]) return UINT64_MAX;
        ExitDestructor *entry = malloc(sizeof *entry);
        if (!entry) return UINT64_MAX;
        Guest *shared = g->master;
        int cxa = !strcmp(name, "__cxa_atexit");
        *entry = (ExitDestructor){a[0], cxa ? a[1] : 0, cxa ? a[2] : 0, NULL};
        pthread_mutex_lock(&shared->once_lock);
        entry->next = shared->exit_destructors; shared->exit_destructors = entry;
        pthread_mutex_unlock(&shared->once_lock);
        return 0;
    }
    if (!strcmp(name, "__cxa_finalize")) {
        if (guest_run_exit_destructors(g, a[0], 0)) *failed = 1;
        return 0;
    }
    if (!strcmp(name, "__register_frame_info") || !strcmp(name, "__deregister_frame_info")) return 0;
    if (!strcmp(name, "__stack_chk_fail") || !strcmp(name, "__assert_fail") || !strcmp(name, "abort")) {
        fprintf(stderr, "Guest called %s\n", name); *failed = 1; return 0;
    }
    uint64_t translated[8];memcpy(translated,a,sizeof translated);
    static const char *const locale_one[]={"tolower_l","toupper_l","iswalpha_l","iswblank_l","iswcntrl_l","iswdigit_l",
        "iswlower_l","iswprint_l","iswpunct_l","iswspace_l","iswupper_l","iswxdigit_l","towlower_l","towupper_l",NULL};
    static const char *const locale_two[]={"strcoll_l","wcscoll_l",NULL};
    static const char *const locale_three[]={"strxfrm_l","wcsxfrm_l",NULL};
    int locale_index=in_names(name,locale_one)?1:in_names(name,locale_two)?2:in_names(name,locale_three)?3:
                     !strcmp(name,"strftime_l")?4:-1;
    if(locale_index>=0)translated[locale_index]=(uintptr_t)host_locale(a[locale_index]);
    // Only imports with the same argument, result, and pointed-to layout on both ABIs
    // reach Darwin through this path. Everything else needs an explicit shim.
    static const char *const identical[] = {
        "access","close","dup","read","write","lseek","pread","getentropy","geteuid","getpid","getpagesize","isatty",
        "fclose","feof","fflush","fgetc","fgets","fileno","fputc","fread","fseek","fseeko","ftell","ftello","fwrite","getc","rewind","ungetc",
        "memchr","memcmp","memcpy","memmove","memset","bcmp","strlen","strnlen","strcmp","strncmp","strchr","strrchr",
        "strcpy","strncpy","strcat","strncat","strstr","wcslen","wmemchr","wmemcmp","mbtowc",
        "__memcpy_chk","__memmove_chk","__memset_chk","__strcpy_chk","__strncpy_chk","__strcat_chk","__strncat_chk",
        "strtol","strtoll","strtoul","strtoull","pthread_self","pthread_join","pthread_detach","pthread_getspecific",
        "pthread_setspecific","pthread_key_delete","nanosleep","sched_yield",NULL
    };
    if(locale_index<0 && !in_names(name,identical)){fprintf(stderr,"Unsupported Linux import: %s\n",name);*failed=1;return 0;}
    void *function = dlsym(RTLD_DEFAULT, name);
    if (!function) {
        fprintf(stderr, "Unimplemented Linux import: %s\n", name);
        *failed = 1; return 0;
    }
    GenericFunction f = (GenericFunction)function;
    // The guest may clear its own errno immediately before calling libc.
    // Carry that value into Darwin's thread-local errno, not a stale host one.
    errno = host_errno(*(int *)(uintptr_t)g->errno_slot);
    uint64_t value = f(translated[0],translated[1],translated[2],translated[3],translated[4],translated[5],translated[6],translated[7]);
    *(int *)(uintptr_t)g->errno_slot = linux_errno(errno);
    if (!strcmp(name, "pthread_join") || !strcmp(name, "pthread_detach") || !strcmp(name, "pthread_setspecific") || !strcmp(name, "pthread_key_delete"))
        value = (uint64_t)linux_errno((int)value);
    return value;
}

int guest_hostcall(Guest *g, unsigned id, const uint64_t args[8], uint64_t *result) {
    Guest *shared=g->master;pthread_mutex_lock(&shared->import_lock);
    if (!id || id > shared->import_count) {pthread_mutex_unlock(&shared->import_lock);
        fprintf(stderr, "Unknown guest host-call %u\n", id); return -1;}
    const char *name = shared->imports[id - 1].name;pthread_mutex_unlock(&shared->import_lock);
    if (g->master->profile) __atomic_add_fetch(&g->master->hostcall_counts[id - 1], 1, __ATOMIC_RELAXED);
    static const char *const noisy[] = {"malloc", "free", "calloc", "realloc", "memcpy", "memmove", "memset",
        "strlen", "strnlen", "memcmp", "memchr", "bcmp", "pthread_mutex_lock", "pthread_mutex_unlock",
        "pthread_getspecific", "__memcpy_chk", "__memmove_chk", "__memset_chk", "rand", "aligned_alloc",
        "posix_memalign", "clock_gettime", "__errno_location", "log2", "cos", NULL};
    int routine_syscall = !strcmp(name, "syscall") && (args[0] == 98 || args[0] == 178);
    if (g->trace && (g->trace > 1 || (!in_names(name, noisy) && !routine_syscall))) fprintf(stderr, "hostcall %s(%#llx, %#llx, %#llx)\n", name,
                          (unsigned long long)args[0], (unsigned long long)args[1],
                          (unsigned long long)args[2]);
    int failed = 0;
    int formatted = linux_vformat(g, name, args, result);
    if (formatted) return formatted < 0 ? -1 : 0;
    int scanned=linux_vscan(g,name,args,result);
    if(scanned)return scanned<0?-1:0;
    if(c23_integer(g,name,args,result))return 0;
    int multibyte=multibyte_call(g,name,args,result);
    if(multibyte)return multibyte<0?-1:0;
    int quad=quad_call(g,name,args);
    if(quad)return quad<0?-1:0;
    int floating = floating_call(g, name, args, result);
    if (floating) return floating < 0 ? -1 : 0;
    *result = invoke(g, name, args, &failed);
    return failed ? -1 : 0;
}
