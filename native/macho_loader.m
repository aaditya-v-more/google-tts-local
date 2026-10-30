#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest.h"
#include "guest_native.h"
#include <mach-o/loader.h>

// Bytes mapped for the image whose Mach-O header is at `header`, excluding __LINKEDIT.
static uintptr_t mapped_end(const struct mach_header_64 *header) {
    const uint8_t *command = (const uint8_t *)(header + 1);
    uint64_t low = UINT64_MAX, high = 0;
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        const struct load_command *lc = (const struct load_command *)command;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
            if (strcmp(seg->segname, "__PAGEZERO") && strcmp(seg->segname, "__LINKEDIT") && seg->vmsize) {
                if (seg->vmaddr < low) low = seg->vmaddr;
                if (seg->vmaddr + seg->vmsize > high) high = seg->vmaddr + seg->vmsize;
            }
        }
        command += lc->cmdsize;
    }
    return high > low ? (uintptr_t)header + (uintptr_t)(high - low) : (uintptr_t)header;
}

static const char *const sources[MAX_OBJECTS] = {
    "libchrometts.so", "runtime/libc++.so.1", "runtime/libc++abi.so.1",
    "runtime/libgcc_s.so.1", "runtime/libunwind.so.1"
};
static const char *const libraries[MAX_OBJECTS] = {
    "libchrometts.dylib", "libcxx.dylib", "libcxxabi.dylib", "libgcc.dylib", "libunwind.dylib"
};

static NSString *sha256(NSString *path) {
    if (!path) return nil;
    FILE *file = fopen(path.fileSystemRepresentation, "rb");
    if (!file) return nil;
    CC_SHA256_CTX context; CC_SHA256_Init(&context);
    unsigned char buffer[65536], digest[CC_SHA256_DIGEST_LENGTH]; size_t length;
    while ((length = fread(buffer, 1, sizeof buffer, file))) CC_SHA256_Update(&context, buffer, (CC_LONG)length);
    int failed = ferror(file); fclose(file);
    if (failed) return nil;
    CC_SHA256_Final(digest, &context);
    char hex[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    for (size_t i = 0; i < sizeof digest; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return [NSString stringWithUTF8String:hex];
}

static int valid_number(NSDictionary *dictionary, NSString *key, uint64_t maximum) {
    id value = dictionary[key];
    return [value isKindOfClass:[NSNumber class]] && [value longLongValue] >= 0 &&
           [value unsignedLongLongValue] <= maximum;
}

static int check_metadata(NSDictionary *root, NSString *data, NSString *dir, NSArray **objectsOut, NSArray **importsOut) {
    if (![root isKindOfClass:[NSDictionary class]] || ![root[@"version"] isEqual:@1]) return -1;
    char executable[PATH_MAX], resolved[PATH_MAX]; uint32_t length = sizeof executable;
    if (_NSGetExecutablePath(executable, &length) || !realpath(executable, resolved)) return -1;
    NSString *bundled = [[NSString stringWithUTF8String:resolved] stringByDeletingLastPathComponent];
    NSString *expectedShim = root[@"shim_sha256"], *expectedConverter = root[@"converter_sha256"];
    if (![expectedShim isKindOfClass:[NSString class]] || ![expectedConverter isKindOfClass:[NSString class]] ||
        ![sha256([dir stringByAppendingPathComponent:@"libgtts-shim.dylib"]) isEqualToString:expectedShim] ||
        ![sha256([bundled stringByAppendingPathComponent:@"libgtts-shim.dylib"]) isEqualToString:expectedShim] ||
        ![sha256([bundled stringByAppendingPathComponent:@"gtts-macho-convert.py"]) isEqualToString:expectedConverter]) return -1;
    NSArray *objects = root[@"objects"], *imports = root[@"imports"];
    if (![objects isKindOfClass:[NSArray class]] || objects.count != MAX_OBJECTS ||
        ![imports isKindOfClass:[NSArray class]] || imports.count > MAX_IMPORTS) return -1;
    NSString *previous = nil;
    for (id value in imports) {
        if (![value isKindOfClass:[NSString class]]) return -1;
        const char *text = [value UTF8String];
        if (!text || ![value length] || [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding] != strlen(text) ||
            (previous && [previous compare:value] != NSOrderedAscending)) return -1;
        previous = value;
    }
    const char *override = getenv("GTTS_NATIVE_LIB");
    for (size_t i = 0; i < MAX_OBJECTS; ++i) {
        id value = objects[i];
        if (![value isKindOfClass:[NSDictionary class]]) return -1;
        NSDictionary *obj = value;
        if (![obj[@"name"] isKindOfClass:[NSString class]] ||
            ![obj[@"source"] isKindOfClass:[NSString class]] ||
            ![obj[@"name"] isEqualToString:[NSString stringWithUTF8String:libraries[i]]] ||
            ![obj[@"source"] isEqualToString:[NSString stringWithUTF8String:sources[i]]] ||
            ![obj[@"sha256"] isKindOfClass:[NSString class]] ||
            ![obj[@"dylib_sha256"] isKindOfClass:[NSString class]] ||
            ![sha256([dir stringByAppendingPathComponent:obj[@"name"]]) isEqualToString:obj[@"dylib_sha256"]]) return -1;
        const char *source = i == 0 && override && *override ? override : NULL;
        NSString *path = source ? [NSString stringWithUTF8String:source] :
                         [data stringByAppendingPathComponent:[NSString stringWithUTF8String:sources[i]]];
        if (![[sha256(path) lowercaseString] isEqualToString:obj[@"sha256"]]) return -1;
        if (!valid_number(obj, @"size", 512 * 1024 * 1024) || ![obj[@"size"] unsignedLongLongValue]) return -1;
        uint64_t size = [obj[@"size"] unsignedLongLongValue];
        for (NSString *key in @[@"phdr", @"dynamic", @"eh_frame", @"init", @"init_array", @"tls_image"])
            if (!valid_number(obj, key, size)) return -1;
        if (!valid_number(obj, @"phnum", 65535) ||
            [obj[@"phnum"] unsignedLongLongValue] * 56 > size - [obj[@"phdr"] unsignedLongLongValue] ||
            !valid_number(obj, @"init_array_size", size - [obj[@"init_array"] unsignedLongLongValue]) ||
            [obj[@"init_array_size"] unsignedLongLongValue] % 8 ||
            !valid_number(obj, @"tls_memsz", 65536) ||
            !valid_number(obj, @"tls_filesz", [obj[@"tls_memsz"] unsignedLongLongValue]) ||
            !valid_number(obj, @"tls_offset", 65536 - [obj[@"tls_memsz"] unsignedLongLongValue]) ||
            [obj[@"tls_filesz"] unsignedLongLongValue] > size - [obj[@"tls_image"] unsignedLongLongValue] ||
            !valid_number(obj, @"tls_align", 65536)) return -1;
    }
    *objectsOut = objects;
    *importsOut = imports;
    return 0;
}

static int use_slot(void *shim, const char *name, uint64_t *slot, size_t count) {
    uint64_t *replacement = dlsym(shim, name);
    if (!replacement) return -1;
    memcpy(replacement, (const void *)(uintptr_t)*slot, count * sizeof(uint64_t));
    *slot = (uintptr_t)replacement;
    return 0;
}

static void close_handles(void **handles, void *shim) {
    for (size_t i = MAX_OBJECTS; i > 0; --i) if (handles[i - 1]) dlclose(handles[i - 1]);
    if (shim) dlclose(shim);
}

/* -2 means the generated set is unavailable or stale; no guest state changed. */
int guest_macho_load(Guest *g) {
    @autoreleasepool {
        const char *dataEnv = getenv("GTTS_DATA"), *dirEnv = getenv("GTTS_MACHO_DIR");
        if (!dataEnv || !*dataEnv) return -2;
        NSString *data = [NSString stringWithUTF8String:dataEnv];
        NSString *dir = dirEnv && *dirEnv ? [NSString stringWithUTF8String:dirEnv] :
                        [data stringByAppendingPathComponent:@"macho"];
        if (!data || !dir) return -2;
        NSData *metadata = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:@"metadata.plist"]];
        id root = metadata ? [NSPropertyListSerialization propertyListWithData:metadata options:0 format:NULL error:NULL] : nil;
        NSArray *objects = nil, *imports = nil;
        if (check_metadata(root, data, dir, &objects, &imports)) {
            if (getenv("GTTS_NATIVE_BACKEND") || g->trace)
                fprintf(stderr, "Mach-O images are unavailable, stale, or invalid; run gtts fetch.\n");
            return -2;
        }
        void *handles[MAX_OBJECTS] = {0};
        NSString *shimPath = [dir stringByAppendingPathComponent:@"libgtts-shim.dylib"];
        void *shim = dlopen(shimPath.fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);
        if (!shim) {
            if (getenv("GTTS_NATIVE_BACKEND") || g->trace) fprintf(stderr, "Cannot load Mach-O shim: %s\n", dlerror());
            return -2;
        }
        void *bases[MAX_OBJECTS] = {0}, *initializers[MAX_OBJECTS] = {0};
        uintptr_t ends[MAX_OBJECTS] = {0};
        for (size_t i = 0; i < MAX_OBJECTS; ++i) {
            NSString *path = [dir stringByAppendingPathComponent:[NSString stringWithUTF8String:libraries[i]]];
            handles[i] = dlopen(path.fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);
            if (!handles[i]) {
                if (getenv("GTTS_NATIVE_BACKEND") || g->trace) fprintf(stderr, "Cannot load %s: %s\n", libraries[i], dlerror());
                close_handles(handles, shim); return -2;
            }
            bases[i] = dlsym(handles[i], "gtts_image_base");
            initializers[i] = dlsym(handles[i], "gtts_explicit_init");
            Dl_info info, initInfo;
            if (!bases[i] || !initializers[i] || !dladdr(bases[i], &info) ||
                !dladdr(initializers[i], &initInfo) || initInfo.dli_fbase != info.dli_fbase ||
                (uintptr_t)bases[i] != (uintptr_t)info.dli_fbase + 0x4000) {
                close_handles(handles, shim); return -2;
            }
            ends[i] = mapped_end((const struct mach_header_64 *)info.dli_fbase);
        }
        unsigned *tlsKey = dlsym(shim, "gtts_tls_key");
        void *entry = dlsym(shim, "native_guest_enter");
        if (!tlsKey || !entry) { close_handles(handles, shim); return -2; }
        *tlsKey = guest_native_tls_key;
        memcpy(g->macho_handles, handles, sizeof handles);
        g->macho_shim = shim;
        g->macho_enter = entry;
        if (use_slot(shim, "gtts_slot_guard", &g->stack_guard, 1) ||
            use_slot(shim, "gtts_slot_stdin", &g->stdin_slot, 1) ||
            use_slot(shim, "gtts_slot_stdout", &g->stdout_slot, 1) ||
            use_slot(shim, "gtts_slot_stderr", &g->stderr_slot, 1) ||
            use_slot(shim, "gtts_slot_environ", &g->environ_slot, 1) ||
            use_slot(shim, "gtts_slot_tzname", &g->tzname_slot, 2) ||
            use_slot(shim, "gtts_slot_timezone", &g->timezone_slot, 1)) return -1;
        for (NSString *name in imports) {
            size_t index = g->import_count;
            char symbol[32]; snprintf(symbol, sizeof symbol, "gtts_h%04zu", index + 1);
            void *address = dlsym(shim, symbol);
            char *copy = strdup(name.UTF8String);
            if (!address || !copy) { free(copy); return -1; }
            g->imports[index] = (Import){.name = copy, .address = (uintptr_t)address};
            ++g->import_count;
        }
        const char *override = getenv("GTTS_NATIVE_LIB");
        for (size_t i = 0; i < MAX_OBJECTS; ++i) {
            NSDictionary *meta = objects[i]; ElfObject *obj = &g->objects[i];
            NSString *path = i == 0 && override && *override ? [NSString stringWithUTF8String:override] :
                [data stringByAppendingPathComponent:[NSString stringWithUTF8String:sources[i]]];
            obj->path = strdup(path.fileSystemRepresentation);
            obj->base = bases[i]; obj->size = [meta[@"size"] unsignedLongLongValue];
            // The recorded size is the ELF extent rounded up; dyld may place the next image
            // inside that slack, so never claim more than this image actually maps.
            if (ends[i] > (uintptr_t)obj->base && obj->size > ends[i] - (uintptr_t)obj->base) obj->size = ends[i] - (uintptr_t)obj->base;
            obj->macho_initializer = (uintptr_t)initializers[i];
            obj->phdr = (uintptr_t)obj->base + [meta[@"phdr"] unsignedLongLongValue];
            obj->phnum = [meta[@"phnum"] unsignedLongLongValue];
            obj->dynamic = (uintptr_t)obj->base + [meta[@"dynamic"] unsignedLongLongValue];
            uint64_t eh = [meta[@"eh_frame"] unsignedLongLongValue];
            obj->eh_frame = eh ? (uintptr_t)obj->base + eh : 0;
            obj->init = [meta[@"init"] unsignedLongLongValue];
            obj->init_array = [meta[@"init_array"] unsignedLongLongValue];
            obj->init_array_size = [meta[@"init_array_size"] unsignedLongLongValue];
            obj->tls_image = [meta[@"tls_image"] unsignedLongLongValue];
            obj->tls_filesz = [meta[@"tls_filesz"] unsignedLongLongValue];
            obj->tls_memsz = [meta[@"tls_memsz"] unsignedLongLongValue];
            obj->tls_align = [meta[@"tls_align"] unsignedLongLongValue];
            obj->tls_offset = [meta[@"tls_offset"] unsignedLongLongValue];
            obj->guest_path = (uintptr_t)guest_alloc(g, strlen(path.fileSystemRepresentation) + 1, 1);
            obj->link_map = (uintptr_t)guest_alloc(g, 5 * sizeof(uint64_t), 16);
            if (!obj->path || !obj->guest_path || !obj->link_map) return -1;
            strcpy((char *)(uintptr_t)obj->guest_path, path.fileSystemRepresentation);
            memset((void *)(uintptr_t)obj->link_map, 0, 5 * sizeof(uint64_t));
            if (obj->tls_filesz) memcpy(g->tls + obj->tls_offset, obj->base + obj->tls_image, obj->tls_filesz);
            if (obj->tls_memsz && obj->tls_offset + obj->tls_memsz > g->tls_used)
                g->tls_used = obj->tls_offset + obj->tls_memsz;
            ++g->object_count;
            if (g->trace) fprintf(stderr, "dyld loaded %s at %p\n", libraries[i], obj->base);
        }
        for (size_t i = 0; i < g->object_count; ++i) {
            ElfObject *obj = &g->objects[i]; uint64_t *map = (uint64_t *)(uintptr_t)obj->link_map;
            map[0] = (uintptr_t)obj->base; map[1] = obj->guest_path; map[2] = obj->dynamic;
            map[3] = i + 1 < g->object_count ? g->objects[i + 1].link_map : 0;
            map[4] = i ? g->objects[i - 1].link_map : 0;
        }
        if (g->trace) fprintf(stderr, "dyld bound %zu images; %zu host imports\n", g->object_count, g->import_count);
        return 0;
    }
}

uint64_t guest_macho_symbol(Guest *g, const char *name) {
    if (!name) return 0;
    size_t length = strlen(name);
    if (length > 65536) return 0;
    char *symbol = malloc(length + sizeof "gtts_c_");
    if (!symbol) return 0;
    memcpy(symbol, "gtts_c_", sizeof "gtts_c_" - 1);
    memcpy(symbol + sizeof "gtts_c_" - 1, name, length + 1);
    uint64_t result = 0;
    for (size_t i = 0; i < MAX_OBJECTS && !result; ++i)
        if (g->macho_handles[i]) result = (uintptr_t)dlsym(g->macho_handles[i], symbol);
    free(symbol);
    return result;
}

void guest_macho_close(Guest *g) {
    close_handles(g->macho_handles, g->macho_shim);
    memset(g->macho_handles, 0, sizeof g->macho_handles);
    g->macho_shim = NULL;
    g->macho_enter = NULL;
}
