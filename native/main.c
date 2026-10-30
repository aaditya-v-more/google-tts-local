#include "guest.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int worker_run(Guest *g, int protocol_fd, const char *pack, int verbose);

int main(int argc, char **argv) {
    double begin = guest_now();
    if (argc < 2 || (strcmp(argv[1], "inspect") && strcmp(argv[1], "init") && strcmp(argv[1], "worker"))) {
        fprintf(stderr, "usage: %s inspect|init|worker [--lib file --pack file --verbose]\n", argv[0]); return 2;
    }
    const char *pack = NULL, *lib = NULL; int verbose = 0, protocol_fd = -1;
    if (!strcmp(argv[1], "worker")) {
        for (int i = 2; i < argc; ++i) {
            if (!strcmp(argv[i], "--verbose")) { verbose = 1; continue; }
            if (i + 1 >= argc) { fprintf(stderr, "Missing argument for %s\n", argv[i]); return 2; }
            if (!strcmp(argv[i], "--pack")) pack = argv[++i];
            else if (!strcmp(argv[i], "--lib")) lib = argv[++i];
            else { fprintf(stderr, "Unknown option %s\n", argv[i]); return 2; }
        }
        if (!pack) { fprintf(stderr, "worker requires --pack\n"); return 2; }
        if (lib) setenv("GTTS_NATIVE_LIB", lib, 1);
        protocol_fd = dup(1); if (protocol_fd < 0 || dup2(2, 1) < 0) return 1;
    }
    if (!getenv("GTTS_DATA")) { fprintf(stderr, "Data directory is missing. Run gtts fetch.\n"); return 1; }
    Guest guest;
    if (guest_create(&guest)) return 1;
    guest.profile = getenv("GTTS_NATIVE_PROFILE") != NULL;
    double created = guest_now();
    const char *trace = getenv("GTTS_NATIVE_TRACE");
    guest.trace = trace ? (!strcmp(trace, "all") ? 2 : 1) : 0;
    int failed = guest_load_objects(&guest);
    double loaded = guest_now();
    if (!failed && !guest_symbol(&guest, "GoogleTtsInit")) {
        fprintf(stderr, "GoogleTtsInit not found in the loaded engine\n"); failed = 1;
    }
    if (!failed && strcmp(argv[1], "inspect")) failed = guest_initialize_objects(&guest);
    double initialized = guest_now();
    if (guest.profile) fprintf(stderr, "PROFILE backend=%s guest_setup=%.3fs image_load=%.3fs constructors=%.3fs\n",
                               guest.macho_mode ? "macho" : guest.native_mode ? "inprocess" : "vm", created - begin, loaded - created, initialized - loaded);
    if (!failed && !strcmp(argv[1], "worker")) failed = worker_run(&guest, protocol_fd, pack, verbose);
    if (!failed && strcmp(argv[1], "worker")) printf("%s: %zu %s objects, %zu imports, GoogleTtsInit=%#llx\n", argv[1],
                        guest.object_count, guest.macho_mode ? "Mach-O" : "ELF", guest.import_count,
                        (unsigned long long)guest_symbol(&guest, "GoogleTtsInit"));
    if (guest.profile) {
        for (unsigned n = 0; n < 15; ++n) {
            uint64_t highest = 0; size_t best = 0;
            for (size_t i = 0; i < guest.import_count; ++i)
                if (guest.hostcall_counts[i] > highest) { highest = guest.hostcall_counts[i]; best = i; }
            if (!highest) break;
            fprintf(stderr, "PROFILE import %s=%llu\n", guest.imports[best].name, (unsigned long long)highest);
            guest.hostcall_counts[best] = 0;
        }
        fprintf(stderr, "PROFILE total=%.3fs arena_used=%zu\n", guest_now() - begin, guest.arena_used);
    }
    guest_destroy(&guest);
    return failed || __atomic_load_n(&guest.broken, __ATOMIC_ACQUIRE) ? 1 : 0;
}
