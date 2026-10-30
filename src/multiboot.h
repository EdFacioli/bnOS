#ifndef BNOS_MULTIBOOT_H
#define BNOS_MULTIBOOT_H

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002u

#define MULTIBOOT_INFO_MEMORY  0x00000001u
#define MULTIBOOT_INFO_CMDLINE 0x00000004u
#define MULTIBOOT_INFO_MMAP    0x00000040u

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
} __attribute__((packed));

struct multiboot_mmap_entry {
    uint32_t size; /* size of this entry, excluding this field */
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed));

#define MULTIBOOT_MMAP_AVAILABLE 1

#endif /* BNOS_MULTIBOOT_H */
