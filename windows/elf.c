// Loads Linux x86-64 shared objects into this process. The engine and its C++
// runtime run directly on the CPU; calls into the C library land in libc.c.
#include "engine.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ElfObject elf_objects[MAX_OBJECTS];
unsigned elf_object_count;

enum { PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7, PT_GNU_EH_FRAME = 0x6474e550 };
enum { DT_NULL = 0, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8, DT_INIT = 12,
       DT_JMPREL = 23, DT_PLTRELSZ = 2, DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27,
       DT_RELRSZ = 35, DT_RELR = 36 };
enum { R_64 = 1, R_GLOB_DAT = 6, R_JUMP_SLOT = 7, R_RELATIVE = 8, R_DTPMOD64 = 16, R_DTPOFF64 = 17 };

typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
                 uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { uint32_t name, type; uint64_t flags, addr, offset, size; uint32_t link, info; uint64_t addralign, entsize; } Shdr;
typedef struct { int64_t tag; uint64_t val; } Dyn;
typedef struct { uint64_t offset, info; int64_t addend; } Rela;

// ---- global symbol table -----------------------------------------------------

#define SYMBOL_SLOTS (1u << 17)
typedef struct { const char *name; void *value; } Definition;
static Definition *definitions;

static uint32_t hash_name(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

// The first object to define a name wins, as with the Linux dynamic linker.
static void define(const char *name, void *value) {
    unsigned pos = hash_name(name) & (SYMBOL_SLOTS - 1);
    while (definitions[pos].name) {
        if (!strcmp(definitions[pos].name, name)) return;
        pos = (pos + 1) & (SYMBOL_SLOTS - 1);
    }
    definitions[pos] = (Definition){name, value};
}

void *elf_symbol(const char *name) {
    if (!definitions) return NULL;
    unsigned pos = hash_name(name) & (SYMBOL_SLOTS - 1);
    while (definitions[pos].name) {
        if (!strcmp(definitions[pos].name, name)) return definitions[pos].value;
        pos = (pos + 1) & (SYMBOL_SLOTS - 1);
    }
    return NULL;
}

// ---- stubs for C library functions this port does not provide ---------------

static uint8_t *stub_page;
static size_t stub_used;

static SYSV void missing_import(const char *name) {
    fprintf(stderr, "The engine called %s, which this Windows port does not implement.\n", name);
    fflush(stderr);
    ExitProcess(70);
}

static void *missing_stub(const char *name) {
    if (!stub_page) {
        stub_page = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!stub_page) return NULL;
    }
    if (stub_used + 32 > 65536) return NULL;
    uint8_t *p = stub_page + stub_used;
    uint64_t text = (uintptr_t)name, target = (uintptr_t)missing_import;
    p[0] = 0x48; p[1] = 0xbf; memcpy(p + 2, &text, 8);      // mov rdi, name
    p[10] = 0x48; p[11] = 0xb8; memcpy(p + 12, &target, 8); // mov rax, missing_import
    p[20] = 0xff; p[21] = 0xe0;                             // jmp rax
    stub_used += 32;
    return p;
}

// ---- code patches -------------------------------------------------------------

// Linux keeps the stack-protector canary at %fs:0x28. Windows has no usable %fs
// in 64-bit code, so the same-length load from %gs:0x30 (the TEB's own address,
// constant for each thread) stands in for it.
static size_t patch_canary_loads(uint8_t *code, size_t size) {
    size_t count = 0;
    for (size_t i = 0; i + 9 <= size; ++i) {
        uint8_t *p = code + i;
        if (p[0] == 0x64 && (p[1] == 0x48 || p[1] == 0x4c) && p[2] == 0x8b && (p[3] & 0xc7) == 0x04 &&
            p[4] == 0x25 && p[5] == 0x28 && !p[6] && !p[7] && !p[8]) {
            p[0] = 0x65; p[5] = 0x30;
            ++count; i += 8;
        }
    }
    return count;
}

// The engine makes five raw Linux system calls (arch_prctl, ioprio_set and
// Google's kernel extensions), all with a fallback when they fail. Each is
// rewritten to return -ENOSYS without leaving the instruction sequence.
typedef struct { uint32_t offset; uint8_t length; uint8_t from[11], to[11]; } CodePatch;
static const CodePatch engine_patches[] = {
    {0x114b8f8, 9, {0xb8,0x9e,0,0,0,0x31,0xd2,0x0f,0x05}, {0x31,0xd2,0x48,0xc7,0xc0,0xda,0xff,0xff,0xff}},
    {0x12881b8, 9, {0x48,0x8b,0x05,0x39,0x07,0x51,0x00,0x0f,0x05}, {0x48,0xc7,0xc0,0xda,0xff,0xff,0xff,0x66,0x90}},
    {0x12888bb, 11, {0x48,0x8b,0x05,0x36,0x00,0x51,0x00,0x31,0xff,0x0f,0x05},
                    {0x31,0xff,0x48,0xc7,0xc0,0xda,0xff,0xff,0xff,0x66,0x90}},
    {0x1288937, 9, {0x48,0x8b,0x05,0xba,0xff,0x50,0x00,0x0f,0x05}, {0x48,0xc7,0xc0,0xda,0xff,0xff,0xff,0x66,0x90}},
    {0x132ace6, 7, {0xb8,0xfb,0,0,0,0x0f,0x05}, {0x48,0xc7,0xc0,0xda,0xff,0xff,0xff}},
};

static int patch_engine_syscalls(ElfObject *obj) {
    for (size_t i = 0; i < sizeof engine_patches / sizeof engine_patches[0]; ++i) {
        const CodePatch *p = &engine_patches[i];
        if (p->offset + p->length > obj->size || memcmp(obj->base + p->offset, p->from, p->length)) {
            fprintf(stderr, "%s is not the expected engine build (system call at %#x).\n", obj->name, p->offset);
            return -1;
        }
        memcpy(obj->base + p->offset, p->to, p->length);
    }
    return 0;
}

// ---- loading -----------------------------------------------------------------

static uint8_t *read_file(const char *path, size_t *size) {
    wchar_t *wide = utf8_to_wide(path);
    HANDLE h = wide ? CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL) : INVALID_HANDLE_VALUE;
    free(wide);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER length;
    uint8_t *data = NULL;
    if (GetFileSizeEx(h, &length) && length.QuadPart > 0 && length.QuadPart < 512ll * 1024 * 1024) {
        data = malloc((size_t)length.QuadPart);
        DWORD got = 0; size_t done = 0;
        while (data && done < (size_t)length.QuadPart) {
            DWORD want = (DWORD)min((size_t)length.QuadPart - done, (size_t)1 << 30);
            if (!ReadFile(h, data + done, want, &got, NULL) || !got) { free(data); data = NULL; break; }
            done += got;
        }
        *size = (size_t)length.QuadPart;
    }
    CloseHandle(h);
    return data;
}

static int inside(size_t total, uint64_t offset, uint64_t size) { return offset <= total && size <= total - offset; }

static unsigned tls_modules;

static int load_object(const char *path, const char *name, int is_engine) {
    size_t length = 0;
    uint8_t *file = read_file(path, &length);
    if (!file) { fprintf(stderr, "Cannot read %s. Run gtts fetch.\n", path); return -1; }
    Ehdr *h = (Ehdr *)file;
    if (length < sizeof *h || memcmp(h->ident, "\177ELF\2\1", 6) || h->type != 3 || h->machine != 62 ||
        h->phentsize != sizeof(ElfPhdr) || h->shentsize != sizeof(Shdr) ||
        !inside(length, h->phoff, (uint64_t)h->phnum * sizeof(ElfPhdr)) ||
        !inside(length, h->shoff, (uint64_t)h->shnum * sizeof(Shdr))) {
        fprintf(stderr, "%s is not an x86-64 Linux shared object.\n", path);
        free(file); return -1;
    }
    ElfPhdr *ph = (ElfPhdr *)(file + h->phoff);
    uint64_t needed = 0;
    for (unsigned i = 0; i < h->phnum; ++i) if (ph[i].type == PT_LOAD) {
        if (!inside(length, ph[i].offset, ph[i].filesz) || ph[i].filesz > ph[i].memsz ||
            ph[i].vaddr + ph[i].memsz > 512ull * 1024 * 1024) {
            fprintf(stderr, "Invalid segment in %s\n", path); free(file); return -1;
        }
        if (ph[i].vaddr + ph[i].memsz > needed) needed = ph[i].vaddr + ph[i].memsz;
    }
    ElfObject *obj = &elf_objects[elf_object_count];
    memset(obj, 0, sizeof *obj);
    snprintf(obj->name, sizeof obj->name, "%s", name);
    obj->size = (size_t)((needed + 0xffff) & ~0xffffull);
    obj->base = VirtualAlloc(NULL, obj->size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!obj->base) { fprintf(stderr, "Out of memory loading %s\n", path); free(file); return -1; }
    obj->phnum = h->phnum;
    obj->phdr = malloc(h->phnum * sizeof(ElfPhdr));
    memcpy(obj->phdr, ph, h->phnum * sizeof(ElfPhdr));
    for (unsigned i = 0; i < h->phnum; ++i) {
        if (ph[i].type == PT_LOAD) memcpy(obj->base + ph[i].vaddr, file + ph[i].offset, (size_t)ph[i].filesz);
        if (ph[i].type == PT_GNU_EH_FRAME) obj->eh_frame = ph[i].vaddr;
        if (ph[i].type == PT_TLS) {
            obj->tls_image = ph[i].vaddr; obj->tls_filesz = ph[i].filesz; obj->tls_memsz = ph[i].memsz;
            obj->tls_align = ph[i].align ? ph[i].align : 1; obj->tls_module = ++tls_modules;
        }
    }
    Shdr *sh = (Shdr *)(file + h->shoff);
    size_t canaries = 0;
    for (unsigned i = 0; i < h->shnum; ++i) {
        if (sh[i].type == 11 && sh[i].entsize == sizeof(ElfSym)) obj->symbol_count = (size_t)(sh[i].size / sizeof(ElfSym));
        if (sh[i].type == 1 && (sh[i].flags & 4)) {   // executable PROGBITS
            if (!inside(obj->size, sh[i].addr, sh[i].size)) { free(file); return -1; }
            canaries += patch_canary_loads(obj->base + sh[i].addr, (size_t)sh[i].size);
        }
    }
    if (getenv("GTTS_TRACE")) fprintf(stderr, "%s: %zu canary loads patched\n", name, canaries);
    free(file);
    if (is_engine && patch_engine_syscalls(obj)) return -1;
    for (unsigned i = 0; i < obj->phnum; ++i) if (obj->phdr[i].type == PT_DYNAMIC) {
        Dyn *dyn = (Dyn *)(obj->base + obj->phdr[i].vaddr);
        for (size_t j = 0; j < obj->phdr[i].memsz / sizeof(Dyn) && dyn[j].tag != DT_NULL; ++j) {
            uint64_t v = dyn[j].val;
            switch (dyn[j].tag) {
            case DT_SYMTAB: obj->symtab = (ElfSym *)(obj->base + v); break;
            case DT_STRTAB: obj->strtab = (const char *)obj->base + v; break;
            case DT_RELA: obj->rela = v; break;
            case DT_RELASZ: obj->rela_size = v; break;
            case DT_JMPREL: obj->jmprel = v; break;
            case DT_PLTRELSZ: obj->jmprel_size = v; break;
            case DT_RELR: obj->relr = v; break;
            case DT_RELRSZ: obj->relr_size = v; break;
            case DT_INIT: obj->init = v; break;
            case DT_INIT_ARRAY: obj->init_array = v; break;
            case DT_INIT_ARRAYSZ: obj->init_array_size = v; break;
            }
        }
    }
    if (!obj->symtab || !obj->strtab || !obj->symbol_count) {
        fprintf(stderr, "No dynamic symbols in %s\n", path); return -1;
    }
    ++elf_object_count;
    return 0;
}

static int relocate(ElfObject *obj, uint64_t offset, uint64_t bytes) {
    if (!bytes) return 0;
    if (!inside(obj->size, offset, bytes) || bytes % sizeof(Rela)) return -1;
    Rela *rel = (Rela *)(obj->base + offset);
    for (size_t i = 0; i < bytes / sizeof(Rela); ++i) {
        unsigned type = (unsigned)rel[i].info;
        uint32_t index = (uint32_t)(rel[i].info >> 32);
        if (rel[i].offset > obj->size - 8 || index >= obj->symbol_count) return -1;
        uint64_t *target = (uint64_t *)(obj->base + rel[i].offset);
        ElfSym *sym = &obj->symtab[index];
        const char *name = obj->strtab + sym->name;
        if (type == R_RELATIVE) { *target = (uintptr_t)obj->base + rel[i].addend; continue; }
        if (type == R_DTPMOD64) {
            if (index && !sym->shndx) { fprintf(stderr, "External TLS symbol %s in %s\n", name, obj->name); return -1; }
            *target = obj->tls_module; continue;
        }
        if (type == R_DTPOFF64) { *target = (index ? sym->value : 0) + rel[i].addend; continue; }
        if (type == R_64 && !index) { *target = (uint64_t)rel[i].addend; continue; }
        if (type != R_64 && type != R_GLOB_DAT && type != R_JUMP_SLOT) {
            fprintf(stderr, "Unsupported relocation %u in %s\n", type, obj->name); return -1;
        }
        unsigned binding = sym->info >> 4, kind = sym->info & 15;
        void *value = NULL;
        if (sym->shndx && (binding == 0 || (sym->other & 3) == 3)) value = obj->base + sym->value;
        if (!value && *name) value = elf_symbol(name);
        if (!value && sym->shndx) value = obj->base + sym->value;
        if (!value && *name) value = shim_data_lookup(name);
        if (!value && *name) value = shim_lookup(name);
        if (!value && binding != 2) {
            if (kind == 1) { fprintf(stderr, "Unsupported Linux data import %s in %s\n", name, obj->name); return -1; }
            value = missing_stub(name);
            if (!value) return -1;
        }
        *target = (uintptr_t)value + (value ? rel[i].addend : 0);
    }
    return 0;
}

static int relocate_relr(ElfObject *obj) {
    if (!obj->relr_size) return 0;
    if (!inside(obj->size, obj->relr, obj->relr_size) || obj->relr_size % 8) return -1;
    uint64_t *entries = (uint64_t *)(obj->base + obj->relr), *where = NULL;
    for (size_t i = 0; i < obj->relr_size / 8; ++i) {
        uint64_t entry = entries[i];
        if (!(entry & 1)) {
            if (entry > obj->size - 8) return -1;
            where = (uint64_t *)(obj->base + entry);
            *where++ += (uintptr_t)obj->base;
        } else {
            if (!where || (uint8_t *)(where + 63) > obj->base + obj->size) return -1;
            for (unsigned b = 0; b < 63; ++b) if ((entry >> (b + 1)) & 1) where[b] += (uintptr_t)obj->base;
            where += 63;
        }
    }
    return 0;
}

static int protect(ElfObject *obj) {
    for (unsigned i = 0; i < obj->phnum; ++i) {
        ElfPhdr *p = &obj->phdr[i];
        if (p->type != PT_LOAD) continue;
        uintptr_t start = ((uintptr_t)obj->base + p->vaddr) & ~(uintptr_t)4095;
        uintptr_t end = ((uintptr_t)obj->base + p->vaddr + p->memsz + 4095) & ~(uintptr_t)4095;
        if (!(p->flags & 1)) continue;
        // A page shared with a writable segment has to stay writable too.
        DWORD mode = PAGE_EXECUTE_READ;
        for (unsigned j = 0; j < obj->phnum; ++j) if (obj->phdr[j].type == PT_LOAD && (obj->phdr[j].flags & 2)) {
            uintptr_t ws = ((uintptr_t)obj->base + obj->phdr[j].vaddr) & ~(uintptr_t)4095;
            uintptr_t we = ((uintptr_t)obj->base + obj->phdr[j].vaddr + obj->phdr[j].memsz + 4095) & ~(uintptr_t)4095;
            if (ws < end && start < we) mode = PAGE_EXECUTE_READWRITE;
        }
        DWORD old;
        if (!VirtualProtect((void *)start, end - start, mode, &old)) {
            fprintf(stderr, "Cannot map code of %s (error %lu)\n", obj->name, GetLastError()); return -1;
        }
    }
    FlushInstructionCache(GetCurrentProcess(), obj->base, obj->size);
    return 0;
}

typedef SYSV void (*InitFunction)(int, char **, char **);

int elf_load_all(const char *data_dir, const char *engine_path) {
    static const char *runtime[] = {"libc++.so.1", "libc++abi.so.1", "libunwind.so.1"};
    char path[1024];
    shim_init();
    definitions = calloc(SYMBOL_SLOTS, sizeof *definitions);
    if (!definitions) return -1;
    if (engine_path) snprintf(path, sizeof path, "%s", engine_path);
    else snprintf(path, sizeof path, "%s/libchrometts.so", data_dir);
    if (load_object(path, "libchrometts.so", 1)) return -1;
    for (size_t i = 0; i < 3; ++i) {
        snprintf(path, sizeof path, "%s/runtime/%s", data_dir, runtime[i]);
        if (load_object(path, runtime[i], 0)) return -1;
    }
    for (unsigned i = 0; i < elf_object_count; ++i) {
        ElfObject *obj = &elf_objects[i];
        for (size_t j = 0; j < obj->symbol_count; ++j) {
            ElfSym *s = &obj->symtab[j];
            unsigned binding = s->info >> 4, kind = s->info & 15;
            if (s->shndx && s->name && (binding == 1 || binding == 2) && (s->other & 3) != 2 && kind != 6)
                define(obj->strtab + s->name, obj->base + s->value);
        }
    }
    for (unsigned i = 0; i < elf_object_count; ++i) {
        ElfObject *obj = &elf_objects[i];
        if (relocate(obj, obj->rela, obj->rela_size) || relocate(obj, obj->jmprel, obj->jmprel_size) || relocate_relr(obj)) {
            fprintf(stderr, "Cannot link %s\n", obj->name); return -1;
        }
    }
    if (stub_page) {
        DWORD old;
        VirtualProtect(stub_page, 65536, PAGE_EXECUTE_READ, &old);
    }
    for (unsigned i = 0; i < elf_object_count; ++i) if (protect(&elf_objects[i])) return -1;
    // Initialise dependencies first: libunwind, libc++abi, libc++, then the engine.
    static char *argv[] = {"gtts", NULL}, *envp[] = {NULL};
    for (unsigned k = elf_object_count; k > 0; --k) {
        ElfObject *obj = &elf_objects[k - 1];
        if (obj->init) ((InitFunction)(obj->base + obj->init))(1, argv, envp);
        uint64_t *array = (uint64_t *)(obj->base + obj->init_array);
        for (size_t i = 0; i < obj->init_array_size / 8; ++i)
            if (array[i] && array[i] != UINT64_MAX) ((InitFunction)(uintptr_t)array[i])(1, argv, envp);
    }
    return 0;
}
