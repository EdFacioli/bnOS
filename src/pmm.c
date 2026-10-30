/*
 * Physical memory manager: one bit per 4 KiB frame, 1 = used, 0 = free.
 * The bitmap is a static array covering the full 32-bit physical range
 * (4 GiB / 4 KiB = 1 MiB frames = 128 KiB of bitmap).
 *
 * Frame 0..255 are always reserved (BIOS, VGA, EBDA, ...) as are the
 * kernel image (including this bitmap) and the multiboot structures.
 * Paging is not enabled, so returned addresses are directly usable.
 */

#include "pmm.h"

#include "lib.h"

#include <stdint.h>

#define PAGE_SIZE 4096u
#define MAX_PAGES (1u << 20) /* 4 GiB / 4 KiB */
#define PAGE_BITMAP_BYTES (MAX_PAGES / 8u)
#define LOW_MEMORY_END 0x00100000u
#define KERNEL_BASE 0x00100000u

static uint8_t page_bitmap[PAGE_BITMAP_BYTES];
static uint8_t alloc_bitmap[PAGE_BITMAP_BYTES];
static uint32_t total_pages;
static uint32_t free_pages;

static inline int bit_test(const uint8_t *bitmap, uint32_t page)
{
    return (bitmap[page >> 3] >> (page & 7)) & 1;
}

static inline void bit_set(uint8_t *bitmap, uint32_t page)
{
    bitmap[page >> 3] |= (uint8_t)(1u << (page & 7));
}

static inline void bit_clear(uint8_t *bitmap, uint32_t page)
{
    bitmap[page >> 3] &= (uint8_t)~(1u << (page & 7));
}

static inline int page_used(uint32_t page)
{
    return bit_test(page_bitmap, page);
}

static inline void page_mark_used(uint32_t page)
{
    bit_set(page_bitmap, page);
}

static inline void page_mark_free(uint32_t page)
{
    bit_clear(page_bitmap, page);
}

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static uint64_t align_down(uint64_t value, uint64_t alignment)
{
    return value & ~(alignment - 1);
}

static void reserve_range(uint64_t start, uint64_t end)
{
    uint64_t limit = (uint64_t)total_pages * PAGE_SIZE;

    if (end > limit)
        end = limit;
    if (start >= end)
        return;

    start = align_down(start, PAGE_SIZE) >> 12;
    end = align_up(end, PAGE_SIZE) >> 12;
    for (uint64_t page = start; page < end; page++)
        page_mark_used((uint32_t)page);
}

static void free_range(uint64_t start, uint64_t end)
{
    uint64_t limit = (uint64_t)total_pages * PAGE_SIZE;

    if (end > limit)
        end = limit;
    if (start >= end)
        return;

    start = align_up(start, PAGE_SIZE) >> 12;
    end = align_down(end, PAGE_SIZE) >> 12;
    for (uint64_t page = start; page < end; page++)
        page_mark_free((uint32_t)page);
}

static uint32_t detect_total_pages(const struct multiboot_info *mbi)
{
    uint64_t highest = 0;

    if (mbi != 0 && (mbi->flags & MULTIBOOT_INFO_MMAP) != 0) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)mbi->mmap_addr;
        const uint8_t *end = p + mbi->mmap_length;

        while (p + sizeof(struct multiboot_mmap_entry) <= end) {
            const struct multiboot_mmap_entry *entry =
                (const struct multiboot_mmap_entry *)p;

            if (entry->type == MULTIBOOT_MMAP_AVAILABLE) {
                uint64_t top = entry->addr + entry->len;
                if (top > highest)
                    highest = top;
            }
            p += entry->size + sizeof(entry->size);
        }
    }

    if (highest == 0 && mbi != 0 && (mbi->flags & MULTIBOOT_INFO_MEMORY) != 0)
        highest = ((uint64_t)mbi->mem_upper + 1024) * 1024; /* KiB -> bytes */

    uint64_t pages = align_down(highest, PAGE_SIZE) >> 12;
    if (pages > MAX_PAGES)
        pages = MAX_PAGES;
    return (uint32_t)pages;
}

static void free_available_ranges(const struct multiboot_info *mbi)
{
    if (mbi == 0)
        return;

    if ((mbi->flags & MULTIBOOT_INFO_MMAP) != 0) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)mbi->mmap_addr;
        const uint8_t *end = p + mbi->mmap_length;

        while (p + sizeof(struct multiboot_mmap_entry) <= end) {
            const struct multiboot_mmap_entry *entry =
                (const struct multiboot_mmap_entry *)p;

            if (entry->type == MULTIBOOT_MMAP_AVAILABLE)
                free_range(entry->addr, entry->addr + entry->len);
            p += entry->size + sizeof(entry->size);
        }
    } else if ((mbi->flags & MULTIBOOT_INFO_MEMORY) != 0) {
        free_range(0, ((uint64_t)mbi->mem_upper + 1024) * 1024);
    }
}

void pmm_init(const struct multiboot_info *mbi, uint32_t kernel_end)
{
    total_pages = detect_total_pages(mbi);
    if (total_pages == 0)
        total_pages = MAX_PAGES;

    /* Everything starts used; only ranges the firmware reports are freed. */
    memset(page_bitmap, 0xFF, sizeof(page_bitmap));
    memset(alloc_bitmap, 0x00, sizeof(alloc_bitmap));
    free_available_ranges(mbi);

    reserve_range(0, LOW_MEMORY_END);
    reserve_range(KERNEL_BASE, kernel_end);

    if (mbi != 0) {
        reserve_range((uint32_t)(uintptr_t)mbi,
                      (uint32_t)(uintptr_t)mbi + sizeof(*mbi));
        if ((mbi->flags & MULTIBOOT_INFO_MMAP) != 0)
            reserve_range(mbi->mmap_addr, (uint64_t)mbi->mmap_addr + mbi->mmap_length);
        if ((mbi->flags & MULTIBOOT_INFO_CMDLINE) != 0 && mbi->cmdline != 0)
            reserve_range(mbi->cmdline,
                          (uint64_t)mbi->cmdline + strlen((const char *)(uintptr_t)mbi->cmdline) + 1);
    }

    free_pages = 0;
    for (uint32_t page = 0; page < total_pages; page++) {
        if (!page_used(page))
            free_pages++;
    }
}

static void *alloc_page(int user_freeable)
{
    for (uint32_t page = 0; page < total_pages; page++) {
        if (!page_used(page)) {
            page_mark_used(page);
            if (user_freeable)
                bit_set(alloc_bitmap, page);
            else
                bit_clear(alloc_bitmap, page);
            free_pages--;
            return (void *)(uintptr_t)(page * PAGE_SIZE);
        }
    }
    return 0;
}

void *pmm_alloc_page(void)
{
    return alloc_page(1);
}

void *pmm_alloc_kernel_page(void)
{
    return alloc_page(0);
}

int pmm_free_page(void *page)
{
    uint32_t address = (uint32_t)(uintptr_t)page;

    if (address == 0 || (address & (PAGE_SIZE - 1)) != 0)
        return 0;

    uint32_t index = address / PAGE_SIZE;
    if (index < (LOW_MEMORY_END / PAGE_SIZE) || index >= total_pages)
        return 0;
    /* Only frames actually handed out by pmm_alloc_page may be freed:
       reserved memory (kernel, bitmap, multiboot) has a used page bit too. */
    if (!bit_test(alloc_bitmap, index))
        return 0;

    bit_clear(alloc_bitmap, index);
    page_mark_free(index);
    free_pages++;
    return 1;
}

uint32_t pmm_total_pages(void)
{
    return total_pages;
}

uint32_t pmm_free_pages(void)
{
    return free_pages;
}
