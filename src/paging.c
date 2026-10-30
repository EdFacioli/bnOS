/*
 * Legacy 32-bit paging: 1024-entry page directory, 1024-entry page tables,
 * 4 KiB pages, identity mapping for all physical memory the PMM manages.
 *
 * The identity map is required because there is no virtual allocator yet:
 * kernel data, page tables, Multiboot structures, VGA memory and every frame
 * returned by pmm_alloc_page stay reachable at their physical address.
 */

#include "paging.h"

#include "pmm.h"
#include "vga.h"

#include <stdint.h>

#define PAGE_SIZE 4096u
#define PTE_PRESENT 0x001u
#define PTE_WRITE   0x002u
#define PDE_PRESENT 0x001u
#define PDE_WRITE   0x002u
#define CR0_PG      0x80000000u

static uint32_t page_directory[1024] __attribute__((aligned(PAGE_SIZE)));
static uint32_t mapped_pages;

void paging_init(uint32_t kernel_end)
{
    uint32_t limit = pmm_total_pages();
    uint32_t minimum = (kernel_end + PAGE_SIZE - 1) / PAGE_SIZE;

    if (minimum < 0x00100000u / PAGE_SIZE)
        minimum = 0x00100000u / PAGE_SIZE; /* low memory: VGA at 0xB8000 */
    if (limit < minimum)
        limit = minimum;

    uint32_t tables = (limit + 1023) / 1024;

    for (uint32_t index = 0; index < tables; index++) {
        uint32_t *table = pmm_alloc_kernel_page();
        if (table == 0) {
            vga_write("paging: out of memory building page tables\n");
            return; /* stays in the loader's paging-off state */
        }

        for (uint32_t entry = 0; entry < 1024; entry++) {
            uint32_t page = index * 1024 + entry;
            if (page < limit) {
                table[entry] = page * PAGE_SIZE | PTE_PRESENT | PTE_WRITE;
                mapped_pages++;
            } else {
                table[entry] = 0;
            }
        }

        /* Physical address: paging is still off while the tables are built. */
        page_directory[index] =
            (uint32_t)(uintptr_t)table | PDE_PRESENT | PDE_WRITE;
    }

    for (uint32_t index = tables; index < 1024; index++)
        page_directory[index] = 0;

    __asm__ volatile("mov %0, %%cr3" : : "r"((uint32_t)(uintptr_t)page_directory)
                     : "memory");

    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= CR0_PG;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

int paging_enabled(void)
{
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    return (cr0 & CR0_PG) != 0;
}

uint32_t paging_directory_address(void)
{
    uint32_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

uint32_t paging_mapped_pages(void)
{
    return mapped_pages;
}

static uint32_t *page_table_for(uint32_t virt, int create)
{
    uint32_t directory_index = virt >> 22;

    if ((page_directory[directory_index] & PDE_PRESENT) == 0) {
        if (!create)
            return 0;

        uint32_t *table = pmm_alloc_kernel_page();
        if (table == 0)
            return 0;
        for (uint32_t entry = 0; entry < 1024; entry++)
            table[entry] = 0;
        page_directory[directory_index] =
            (uint32_t)(uintptr_t)table | PDE_PRESENT | PDE_WRITE;
    }

    return (uint32_t *)(uintptr_t)(page_directory[directory_index] & 0xFFFFF000u);
}

int paging_is_mapped(uint32_t virt)
{
    uint32_t *table = page_table_for(virt, 0);

    if (table == 0)
        return 0;
    return (table[(virt >> 12) & 0x3FF] & PTE_PRESENT) != 0;
}

int paging_map_page(uint32_t virt, uint32_t phys, int writable)
{
    if ((virt & (PAGE_SIZE - 1)) != 0 || (phys & (PAGE_SIZE - 1)) != 0)
        return 0;

    uint32_t *table = page_table_for(virt, 1);
    if (table == 0)
        return 0;

    uint32_t *entry = &table[(virt >> 12) & 0x3FF];
    if ((*entry & PTE_PRESENT) == 0)
        mapped_pages++;

    *entry = (phys & 0xFFFFF000u) | PTE_PRESENT;
    if (writable)
        *entry |= PTE_WRITE;

    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    return 1;
}

int paging_unmap_page(uint32_t virt)
{
    if ((virt & (PAGE_SIZE - 1)) != 0)
        return 0;

    uint32_t *table = page_table_for(virt, 0);
    if (table == 0)
        return 0;

    uint32_t *entry = &table[(virt >> 12) & 0x3FF];
    if ((*entry & PTE_PRESENT) == 0)
        return 0;

    *entry = 0;
    if (mapped_pages > 0)
        mapped_pages--;

    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    return 1;
}
