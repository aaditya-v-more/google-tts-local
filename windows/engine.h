#ifndef GTTS_WIN_ENGINE_H
#define GTTS_WIN_ENGINE_H
#include <stddef.h>
#include <stdint.h>

// Functions the Linux code calls, or that call into it, use the System V ABI.
#define SYSV __attribute__((sysv_abi))

typedef struct {
    uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size;
} ElfSym;

typedef struct {
    uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align;
} ElfPhdr;

typedef struct {
    char name[32];
    uint8_t *base;
    size_t size;
    ElfPhdr *phdr;
    unsigned phnum;
    ElfSym *symtab;
    const char *strtab;
    size_t symbol_count;
    uint64_t rela, rela_size, jmprel, jmprel_size, relr, relr_size;
    uint64_t init, init_array, init_array_size, eh_frame;
    uint64_t tls_image, tls_filesz, tls_memsz, tls_align;
    unsigned tls_module;
} ElfObject;

enum { MAX_OBJECTS = 4 };
extern ElfObject elf_objects[MAX_OBJECTS];
extern unsigned elf_object_count;

// Loads the engine and its C++ runtime from the data directory and runs their
// initialisers. Returns 0 or prints a reason and returns -1.
int elf_load_all(const char *data_dir, const char *engine_path);
void *elf_symbol(const char *name);

// libc.c
void *shim_lookup(const char *name);
void *shim_data_lookup(const char *name);
void shim_init(void);
void *tls_block(unsigned module, uint64_t offset);
void shim_thread_exit(void);
int guest_fprintf_stderr(const char *format, ...);

// format.c: printf and scanf over a System V va_list.
typedef struct { uint32_t gp_offset, fp_offset; uint8_t *overflow, *reg_save; } SysvVaList;
typedef struct { char *data; size_t size, capacity; } TextBuffer;
int sysv_format(TextBuffer *out, const char *format, SysvVaList *args);
int sysv_scan(const char *input, const char *format, SysvVaList *args);
int sysv_wformat(TextBuffer *out, const uint32_t *format, SysvVaList *args);

// Engine logger: collects "speaker" lines while a pack initialises.
extern int engine_verbose;
typedef struct { char name[64], gender[32]; } Speaker;
extern Speaker engine_speakers[128];
extern unsigned engine_speaker_count;

// util
wchar_t *utf8_to_wide(const char *s);
char *wide_to_utf8(const wchar_t *s);
#endif
