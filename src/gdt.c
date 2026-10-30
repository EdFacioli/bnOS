/*
 * Global Descriptor Table: flat 4 GiB kernel code and data segments.
 * There is no user mode yet, so no user segments or TSS are installed.
 */

#include "gdt.h"

#include <stdint.h>

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

enum {
    GDT_ENTRY_NULL = 0,
    GDT_ENTRY_CODE = 1,
    GDT_ENTRY_DATA = 2,
    GDT_ENTRIES = 3,
};

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr gdt_descriptor;

extern void gdt_flush(uint32_t descriptor);

static void gdt_set_entry(int index, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t granularity)
{
    gdt[index].base_low = (uint16_t)(base & 0xFFFF);
    gdt[index].base_mid = (uint8_t)((base >> 16) & 0xFF);
    gdt[index].base_high = (uint8_t)((base >> 24) & 0xFF);
    gdt[index].limit_low = (uint16_t)(limit & 0xFFFF);
    gdt[index].granularity =
        (uint8_t)(((limit >> 16) & 0x0F) | (granularity & 0xF0));
    gdt[index].access = access;
}

void gdt_init(void)
{
    gdt_set_entry(GDT_ENTRY_NULL, 0, 0, 0x00, 0x00);
    /* 0x9A: present, ring 0, code, executable/readable. */
    gdt_set_entry(GDT_ENTRY_CODE, 0, 0xFFFFF, 0x9A, 0xCF);
    /* 0x92: present, ring 0, data, read/write. */
    gdt_set_entry(GDT_ENTRY_DATA, 0, 0xFFFFF, 0x92, 0xCF);

    gdt_descriptor.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_descriptor.base = (uint32_t)(uintptr_t)&gdt;

    gdt_flush((uint32_t)(uintptr_t)&gdt_descriptor);
}
