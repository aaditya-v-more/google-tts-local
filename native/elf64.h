#ifndef GTTS_ELF64_H
#define GTTS_ELF64_H
#include <stdint.h>

typedef struct {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Ehdr;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Phdr;
typedef struct { uint32_t name, type; uint64_t flags, addr, offset, size; uint32_t link, info; uint64_t align, entsize; } Shdr;
typedef struct { uint32_t name; unsigned char info, other; uint16_t shndx; uint64_t value, size; } Sym;
typedef struct { int64_t tag; uint64_t val; } Dyn;
typedef struct { uint64_t offset, info; int64_t addend; } Rela;

enum { PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7, PT_GNU_EH_FRAME = 0x6474e550, SHT_DYNSYM = 11 };
enum {
    DT_NULL=0, DT_PLTRELSZ=2, DT_STRTAB=5, DT_SYMTAB=6, DT_RELA=7, DT_RELASZ=8,
    DT_INIT=12, DT_JMPREL=23, DT_INIT_ARRAY=25, DT_INIT_ARRAYSZ=27,
    DT_RELRSZ=35, DT_RELR=36
};
enum { R_ABS64=257, R_GLOB_DAT=1025, R_JUMP_SLOT=1026, R_RELATIVE=1027, R_TLSDESC=1031 };
#endif
