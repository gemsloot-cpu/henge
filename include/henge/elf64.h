/* Minimal ELF64 / AArch64 definitions used by henge (macOS has no <elf.h>). */
#ifndef HENGE_ELF64_H
#define HENGE_ELF64_H

#include <stdint.h>

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

typedef struct {
    int64_t d_tag;
    uint64_t d_val;
} Elf64_Dyn;

typedef struct {
    uint32_t st_name;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
} Elf64_Rela;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} Elf64_Shdr;

typedef struct {
    uint32_t n_namesz;
    uint32_t n_descsz;
    uint32_t n_type;
} Elf64_Nhdr;

#define ELFMAG0 0x7f
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_DYN 3
#define EM_AARCH64 183

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_NOTE 4
#define PT_PHDR 6
#define PT_TLS 7
#define PT_GNU_RELRO 0x6474e552

#define SHT_PROGBITS 1
#define SHF_EXECINSTR 0x4

#define PF_X 1
#define PF_W 2
#define PF_R 4

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_INIT 12
#define DT_FINI 13
#define DT_SONAME 14
#define DT_TEXTREL 22
#define DT_JMPREL 23
#define DT_PLTREL 20
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_FLAGS 30
#define DT_RELRSZ 35
#define DT_RELR 36
#define DT_RELRENT 37
#define DT_GNU_HASH 0x6ffffef5
#define DT_ANDROID_REL 0x6000000f
#define DT_ANDROID_RELSZ 0x60000010
#define DT_ANDROID_RELA 0x60000011
#define DT_ANDROID_RELASZ 0x60000012
#define DT_ANDROID_RELR 0x6fffe000
#define DT_ANDROID_RELRSZ 0x6fffe001
#define DT_ANDROID_RELRENT 0x6fffe003

#define DF_TEXTREL 0x4

#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define STT_FUNC 2
#define STT_TLS 6
#define STT_GNU_IFUNC 10
#define SHN_UNDEF 0
#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0xf)
#define ELF64_R_SYM(i) ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i) ((uint32_t)(i))

#define NT_GNU_BUILD_ID 3

#define R_AARCH64_NONE 0
#define R_AARCH64_ABS64 257
#define R_AARCH64_GLOB_DAT 1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE 1027
#define R_AARCH64_TLS_TPREL64 1030
#define R_AARCH64_TLSDESC 1031
#define R_AARCH64_IRELATIVE 1032

#endif
