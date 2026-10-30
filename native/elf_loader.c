#include "guest.h"
#include "elf64.h"
#include "guest_native.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define SYMBOL_SLOTS 65536
typedef struct { const char *name; uint64_t value; bool weak; } Definition;
static Definition definitions[SYMBOL_SLOTS];

static uint32_t hash_name(const char *s) {
    uint32_t hash = 2166136261u;
    for (; *s; ++s) hash = (hash ^ (unsigned char)*s) * 16777619u;
    return hash;
}

static void add_definition(const char *name, uint64_t value, bool weak) {
    unsigned pos = hash_name(name) % SYMBOL_SLOTS;
    while (definitions[pos].name) {
        if (!strcmp(definitions[pos].name, name)) {
            if (definitions[pos].weak && !weak) definitions[pos] = (Definition){name, value, weak};
            return;
        }
        pos = (pos + 1) % SYMBOL_SLOTS;
    }
    definitions[pos] = (Definition){name, value, weak};
}

uint64_t guest_symbol(Guest *g, const char *name) {
    if (g->macho_mode) return guest_macho_symbol(g, name);
    unsigned pos = hash_name(name) % SYMBOL_SLOTS;
    while (definitions[pos].name) {
        if (!strcmp(definitions[pos].name, name)) return definitions[pos].value;
        pos = (pos + 1) % SYMBOL_SLOTS;
    }
    return 0;
}

static int in_file(size_t total, uint64_t offset, uint64_t size) {
    return offset <= total && size <= total - offset;
}

static int branch_instruction(uint8_t *from, uint8_t *to, uint32_t *instruction) {
    intptr_t delta = (intptr_t)(uintptr_t)to - (intptr_t)(uintptr_t)from;
    if ((delta & 3) || delta < -(INT64_C(1) << 27) || delta >= (INT64_C(1) << 27)) return -1;
    *instruction = 0x14000000u | ((uint32_t)(delta >> 2) & 0x03ffffffu);
    return 0;
}

static int patch_native_code(Guest *g, ElfObject *obj, const Shdr *sh, unsigned count, size_t file_size) {
    size_t tpidr_count = 0, svc_count = 0;
    int expected_engine_syscall = g->object_count == 0 && obj->size > 0x12a30b8u;
    if (expected_engine_syscall) {
        uint32_t *site = (uint32_t *)(obj->base + 0x12a30b4u);
        if (*site != 0xd4000001u || site[-1] != 0x528003c8u) {
            fprintf(stderr, "Expected Linux syscall sequence is missing in %s\n", obj->path);
            return -1;
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        if (sh[i].type != 1 || !(sh[i].flags & 4)) continue; /* executable PROGBITS */
        if ((sh[i].addr & 3) || (sh[i].size & 3) || !in_file(file_size, sh[i].offset, sh[i].size) ||
            sh[i].addr > obj->size || sh[i].size > obj->size - sh[i].addr) return -1;
        Phdr *ph = (Phdr *)(uintptr_t)obj->phdr;
        int executable_segment = 0;
        for (size_t k = 0; k < obj->phnum; ++k) {
            if (ph[k].type == PT_LOAD && (ph[k].flags & 1) && sh[i].addr >= ph[k].vaddr &&
                sh[i].addr - ph[k].vaddr <= ph[k].filesz &&
                sh[i].size <= ph[k].filesz - (sh[i].addr - ph[k].vaddr) &&
                sh[i].offset == ph[k].offset + (sh[i].addr - ph[k].vaddr)) executable_segment = 1;
        }
        if (!executable_segment) return -1;
        uint32_t *words = (uint32_t *)(obj->base + sh[i].addr);
        for (size_t j = 0; j < sh[i].size / 4; ++j) {
            uint32_t insn = words[j];
            if ((insn & ~31u) == 0xd53bd040u) { /* mrs xN, tpidr_el0 */
                unsigned reg = insn & 31u;
                if (reg == 31 || obj->native_stub_used + 12 > obj->native_stub_size) return -1;
                uint32_t *stub = (uint32_t *)(obj->native_stubs + obj->native_stub_used);
                uint32_t out, back;
                if (branch_instruction((uint8_t *)&words[j], (uint8_t *)stub, &out) ||
                    branch_instruction((uint8_t *)&stub[2], (uint8_t *)&words[j + 1], &back)) return -1;
                stub[0] = 0xd53bd060u | reg; /* mrs xN, tpidrro_el0 */
                stub[1] = 0xf9400000u | (guest_native_tls_key << 10) | (reg << 5) | reg;
                stub[2] = back;
                words[j] = out;
                obj->native_stub_used += 12;
                ++tpidr_count;
            } else if ((insn & 0xffe0001fu) == 0xd4000001u) { /* svc, any immediate */
                uint64_t address = sh[i].addr + j * 4;
                if (g->object_count != 0 || address != 0x12a30b4 || insn != 0xd4000001u ||
                    !j || words[j - 1] != 0x528003c8u) {
                    fprintf(stderr, "Unexpected Linux syscall in %s at %#llx\n", obj->path, (unsigned long long)address);
                    return -1;
                }
                words[j] = 0x928004a0u; /* movn x0, #37: Linux -ENOSYS */
                ++svc_count;
            }
        }
    }
    if (expected_engine_syscall && svc_count != 1) return -1;
    if (g->trace) fprintf(stderr, "patched %s: %zu TLS reads, %zu syscalls\n", obj->path, tpidr_count, svc_count);
    return 0;
}

static int protect_native_object(ElfObject *obj) {
    size_t page = (size_t)getpagesize();
    Phdr *ph = (Phdr *)(uintptr_t)obj->phdr;
    for (size_t i = 0; i < obj->phnum; ++i) {
        if (ph[i].type != PT_LOAD || !(ph[i].flags & 1)) continue;
        uintptr_t start = ((uintptr_t)obj->base + ph[i].vaddr) & ~(page - 1);
        uintptr_t end = ((uintptr_t)obj->base + ph[i].vaddr + ph[i].memsz + page - 1) & ~(page - 1);
        for (size_t j = 0; j < obj->phnum; ++j) {
            if (ph[j].type != PT_LOAD || !(ph[j].flags & 2)) continue;
            uintptr_t writable_start = ((uintptr_t)obj->base + ph[j].vaddr) & ~(page - 1);
            uintptr_t writable_end = ((uintptr_t)obj->base + ph[j].vaddr + ph[j].memsz + page - 1) & ~(page - 1);
            if (start < writable_end && writable_start < end) {
                fprintf(stderr, "Writable and executable segments share a page in %s\n", obj->path); return -1;
            }
        }
        __builtin___clear_cache((char *)start, (char *)end);
        if (mprotect((void *)start, end - start, PROT_READ | PROT_EXEC)) { perror("mprotect ELF code"); return -1; }
    }
    __builtin___clear_cache((char *)obj->native_stubs, (char *)obj->native_stubs + obj->native_stub_used);
    if (mprotect(obj->native_stubs, obj->native_stub_size, PROT_READ | PROT_EXEC)) { perror("mprotect TLS stubs"); return -1; }
    return 0;
}

static int load_object(Guest *g, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1; }
    struct stat st;
    if (fstat(fd, &st) || st.st_size < (off_t)sizeof(Ehdr)) { close(fd); return -1; }
    size_t length = (size_t)st.st_size;
    uint8_t *file = mmap(NULL, length, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED) { perror("mmap ELF"); return -1; }
    Ehdr *h = (Ehdr *)file;
    if (memcmp(h->ident, "\177ELF\2\1", 6) || h->type != 3 || h->machine != 183 ||
        h->phentsize != sizeof(Phdr) || h->shentsize != sizeof(Shdr) ||
        !in_file(length, h->phoff, (uint64_t)h->phnum * sizeof(Phdr)) ||
        !in_file(length, h->shoff, (uint64_t)h->shnum * sizeof(Shdr))) {
        fprintf(stderr, "Invalid aarch64 shared object: %s\n", path);
        munmap(file, length); return -1;
    }
    Phdr *ph = (Phdr *)(file + h->phoff);
    size_t needed = 0;
    for (unsigned i = 0; i < h->phnum; ++i) if (ph[i].type == PT_LOAD) {
        if (!in_file(length, ph[i].offset, ph[i].filesz) || ph[i].filesz > ph[i].memsz ||
            ph[i].vaddr + ph[i].memsz < ph[i].vaddr || ph[i].vaddr + ph[i].memsz > 512u * 1024u * 1024u) {
            fprintf(stderr, "Invalid ELF segment: %s\n", path); munmap(file, length); return -1;
        }
        if (ph[i].vaddr + ph[i].memsz > needed) needed = (size_t)(ph[i].vaddr + ph[i].memsz);
    }
    ElfObject *obj = &g->objects[g->object_count];
    obj->path = strdup(path);
    if (!obj->path) { munmap(file, length); return -1; }
    obj->size = (needed + 65535) & ~(size_t)65535;
    obj->native_stub_size = g->native_mode ? 65536 : 0;
    obj->base = guest_alloc(g, obj->size + obj->native_stub_size, 65536);
    if (!obj->base) { fprintf(stderr, "Guest memory exhausted loading %s\n", path); munmap(file, length); return -1; }
    if (g->native_mode) obj->native_stubs = obj->base + obj->size;
    obj->phnum=h->phnum;obj->phdr=(uintptr_t)guest_alloc(g,(size_t)h->phnum*sizeof(Phdr),16);
    obj->guest_path=(uintptr_t)guest_alloc(g,strlen(path)+1,1);obj->link_map=(uintptr_t)guest_alloc(g,5*sizeof(uint64_t),16);
    if(!obj->phdr || !obj->guest_path || !obj->link_map){munmap(file,length);return -1;}
    memcpy((void *)(uintptr_t)obj->phdr,ph,(size_t)h->phnum*sizeof(Phdr));
    strcpy((char *)(uintptr_t)obj->guest_path,path);memset((void *)(uintptr_t)obj->link_map,0,5*sizeof(uint64_t));
    for (unsigned i = 0; i < h->phnum; ++i) {
        if (ph[i].type == PT_LOAD) memcpy(obj->base + ph[i].vaddr, file + ph[i].offset, (size_t)ph[i].filesz);
        if (ph[i].type == PT_DYNAMIC) obj->dynamic=(uintptr_t)obj->base+ph[i].vaddr;
        if (ph[i].type == PT_GNU_EH_FRAME) obj->eh_frame=(uintptr_t)obj->base+ph[i].vaddr;
        if (ph[i].type == PT_TLS) {
            obj->tls_image = ph[i].vaddr;
            obj->tls_filesz = ph[i].filesz;
            obj->tls_memsz = ph[i].memsz;
            obj->tls_align = ph[i].align ? ph[i].align : 1;
            g->tls_used = (g->tls_used + obj->tls_align - 1) & ~(obj->tls_align - 1);
            obj->tls_offset = g->tls_used;
            if (obj->tls_memsz > 65536 - g->tls_used) { munmap(file, length); return -1; }
            g->tls_used += obj->tls_memsz;
        }
    }
    Shdr *sh = (Shdr *)(file + h->shoff);
    if (g->native_mode && patch_native_code(g, obj, sh, h->shnum, length)) {
        fprintf(stderr, "Native instruction patch failed: %s\n", path); munmap(file, length); return -1;
    }
    for (unsigned i = 0; i < h->shnum; ++i)
        if (sh[i].type == SHT_DYNSYM && sh[i].entsize == sizeof(Sym))
            obj->symbol_count = (size_t)(sh[i].size / sizeof(Sym));
    for (unsigned i = 0; i < h->phnum; ++i) if (ph[i].type == PT_DYNAMIC) {
        Dyn *dyn = (Dyn *)(obj->base + ph[i].vaddr);
        size_t count = (size_t)(ph[i].memsz / sizeof(Dyn));
        for (size_t j = 0; j < count && dyn[j].tag != DT_NULL; ++j) {
            uint64_t v = dyn[j].val;
            switch (dyn[j].tag) {
            case DT_SYMTAB: obj->symtab = obj->base + v; break;
            case DT_STRTAB: obj->strtab = (char *)obj->base + v; break;
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
    munmap(file, length);
    if (!obj->symtab || !obj->strtab || !obj->symbol_count) {
        fprintf(stderr, "Missing dynamic symbols in %s\n", path); return -1;
    }
    ++g->object_count;
    if (g->trace) fprintf(stderr, "mapped %s at %p (%zu bytes, %zu symbols)\n",
                          path, obj->base, obj->size, obj->symbol_count);
    return 0;
}

static uint64_t data_symbol(Guest *g, const char *name) {
    if (!strcmp(name, "__stack_chk_guard")) return g->stack_guard;
    if (!strcmp(name, "stdin")) return g->stdin_slot;
    if (!strcmp(name, "stdout")) return g->stdout_slot;
    if (!strcmp(name, "stderr")) return g->stderr_slot;
    if (!strcmp(name, "environ") || !strcmp(name, "__environ")) return g->environ_slot;
    if (!strcmp(name, "tzname")) return g->tzname_slot;
    if (!strcmp(name, "__timezone") || !strcmp(name, "timezone")) return g->timezone_slot;
    return 0;
}

static int relocate_rela(Guest *g, ElfObject *obj, uint64_t offset, uint64_t bytes) {
    if (!offset && !bytes) return 0;
    if (offset > obj->size || bytes > obj->size - offset || bytes % sizeof(Rela)) return -1;
    Rela *rel = (Rela *)(obj->base + offset);
    Sym *symbols = obj->symtab;
    for (size_t i = 0; i < bytes / sizeof(Rela); ++i) {
        unsigned type = (unsigned)rel[i].info;
        uint32_t index = (uint32_t)(rel[i].info >> 32);
        if (rel[i].offset > obj->size - (type == R_TLSDESC ? 16 : 8) || index >= obj->symbol_count) return -1;
        uint64_t *target = (uint64_t *)(obj->base + rel[i].offset);
        if (type == R_RELATIVE) { *target = (uintptr_t)obj->base + rel[i].addend; continue; }
        if (type == R_TLSDESC) {
            if (index != 0) { fprintf(stderr, "Unsupported external TLS in %s\n", obj->path); return -1; }
            target[0] = g->tls_stub;
            target[1] = obj->tls_offset + rel[i].addend;
            continue;
        }
        if (type != R_ABS64 && type != R_GLOB_DAT && type != R_JUMP_SLOT) {
            fprintf(stderr, "Unsupported relocation %u in %s\n", type, obj->path); return -1;
        }
        Sym *symbol = &symbols[index];
        const char *name = obj->strtab + symbol->name;
        unsigned binding = symbol->info >> 4;
        uint64_t value = 0;
        if (symbol->shndx && (binding == 0 || (symbol->other & 3) != 0))
            value = (uintptr_t)obj->base + symbol->value;
        else value = guest_symbol(g, name);
        if (!value && symbol->shndx) value = (uintptr_t)obj->base + symbol->value;
        if (!value) value = data_symbol(g, name);
        if (!value && binding != 2 && (symbol->info & 15)==1) {
            fprintf(stderr,"Unsupported Linux data import: %s\n",name);return -1;
        }
        if (!value && binding != 2) value = guest_import(g, name);
        if (!value && binding != 2) { fprintf(stderr, "Could not resolve %s\n", name); return -1; }
        *target = value + rel[i].addend;
    }
    return 0;
}

static int relocate_relr(ElfObject *obj) {
    if (!obj->relr_size) return 0;
    if (obj->relr > obj->size || obj->relr_size > obj->size - obj->relr || obj->relr_size % 8) return -1;
    uint64_t *entries = (uint64_t *)(obj->base + obj->relr), *where = NULL;
    for (size_t i = 0; i < obj->relr_size / 8; ++i) {
        uint64_t entry = entries[i];
        if (!(entry & 1)) {
            if (entry > obj->size - 8) return -1;
            where = (uint64_t *)(obj->base + entry);
            *where++ += (uintptr_t)obj->base;
        } else {
            if (!where || (uint8_t *)(where + 63) > obj->base + obj->size) return -1;
            uint64_t bits = entry >> 1;
            for (unsigned b = 0; b < 63; ++b) if (bits & (UINT64_C(1) << b)) where[b] += (uintptr_t)obj->base;
            where += 63;
        }
    }
    return 0;
}

int guest_load_objects(Guest *g) {
    if (g->macho_mode) {
        int status = guest_macho_load(g);
        const char *backend = getenv("GTTS_NATIVE_BACKEND");
        if (status != -2 || (backend && *backend)) return status;
        if (g->trace) fprintf(stderr, "Mach-O conversion unavailable; using in-process ELF fallback\n");
        g->macho_mode = 0;
    }
    const char *data=getenv("GTTS_DATA");
    if (!data || !*data) { fprintf(stderr, "GTTS_DATA is missing. Run gtts fetch.\n"); return -1; }
    static const char *names[] = {"libchrometts.so", "runtime/libc++.so.1", "runtime/libc++abi.so.1", "runtime/libgcc_s.so.1", "runtime/libunwind.so.1"};
    char paths[5][PATH_MAX];
    for (size_t i=0;i<5;++i) if (snprintf(paths[i],sizeof paths[i],"%s/%s",data,names[i]) >= (int)sizeof paths[i]) return -1;
    const char *engine_path = getenv("GTTS_NATIVE_LIB");
    memset(definitions, 0, sizeof(definitions));
    for (size_t i = 0; i < 5; ++i)
        if (load_object(g, i==0 && engine_path && *engine_path ? engine_path : paths[i])) return -1;
    for(size_t i=0;i<g->object_count;++i){ElfObject *obj=&g->objects[i];uint64_t *map=(uint64_t *)(uintptr_t)obj->link_map;
        map[0]=(uintptr_t)obj->base;map[1]=obj->guest_path;map[2]=obj->dynamic;
        map[3]=i+1<g->object_count?g->objects[i+1].link_map:0;map[4]=i?g->objects[i-1].link_map:0;}
    for (size_t i = 0; i < g->object_count; ++i) {
        ElfObject *obj = &g->objects[i];
        Sym *symbols = obj->symtab;
        for (size_t j = 0; j < obj->symbol_count; ++j) {
            Sym *s = &symbols[j];
            unsigned binding = s->info >> 4, type = s->info & 15;
            if (s->shndx && s->name && (binding == 1 || binding == 2) &&
                (s->other & 3) != 2 && type != 6)
                add_definition(obj->strtab + s->name, (uintptr_t)obj->base + s->value, binding == 2);
        }
    }
    for (size_t i = 0; i < g->object_count; ++i) {
        ElfObject *obj = &g->objects[i];
        if (relocate_rela(g, obj, obj->rela, obj->rela_size) ||
            relocate_rela(g, obj, obj->jmprel, obj->jmprel_size) || relocate_relr(obj)) {
            fprintf(stderr, "Relocation failed: %s\n", obj->path); return -1;
        }
    }
    // The main thread's TLS starts as a copy of each object's template, taken after
    // relocation so pointers in .tdata are final (later threads copy g->tls).
    for (size_t i = 0; i < g->object_count; ++i) {
        ElfObject *obj = &g->objects[i];
        if (obj->tls_filesz) memcpy(g->tls + obj->tls_offset, obj->base + obj->tls_image, (size_t)obj->tls_filesz);
    }
    if (g->native_mode) for (size_t i = 0; i < g->object_count; ++i)
        if (protect_native_object(&g->objects[i])) return -1;
    if (g->trace) fprintf(stderr, "relocated %zu objects; %zu host imports\n", g->object_count, g->import_count);
    return 0;
}

static int run_initializer(Guest *g, ElfObject *obj, uint64_t function, uint64_t args[8], uint64_t *result) {
    if (!g->macho_mode) return guest_call(g, function, args, result);
    if (!obj->macho_initializer) return -1;
    /* The dylib entry tail-calls x3 after the shim has enabled the guest ABI. */
    args[3] = function;
    int status = guest_call(g, obj->macho_initializer, args, result);
    args[3] = 0;
    return status;
}

int guest_initialize_objects(Guest *g) {
    // Dependencies before the engine. libunwind is a dependency of libc++abi.
    uint64_t arguments[8]={1,g->argv,*(uint64_t *)(uintptr_t)g->environ_slot};
    for(size_t k=g->object_count;k>0;--k){ElfObject *obj=&g->objects[k-1];uint64_t ignored;
        if(obj->init && run_initializer(g,obj,(uintptr_t)obj->base+obj->init,arguments,&ignored)){
            fprintf(stderr,"DT_INIT failed: %s\n",obj->path);return -1;}
        uint64_t *array=(uint64_t *)(obj->base+obj->init_array);
        for(size_t i=0;i<obj->init_array_size/8;++i){
            if(g->trace)fprintf(stderr,"init %s[%zu] at %#llx\n",obj->path,i,(unsigned long long)array[i]);
            if(array[i] && array[i]!=UINT64_MAX && run_initializer(g,obj,array[i],arguments,&ignored)){
                fprintf(stderr,"Initializer %zu failed: %s\n",i,obj->path);return -1;}
        }
    }return 0;
}
