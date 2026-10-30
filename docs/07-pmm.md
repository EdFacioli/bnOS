# 7. Physical memory manager

Files: `src/pmm.c`, `src/pmm.h`, `src/multiboot.h`.

The PMM hands out 4 KiB physical frames. It keeps two bitmaps covering the
whole 32-bit physical address space: one bit per frame for "used", and a second
bit for "handed out by `pmm_alloc_page` and therefore freeable by the shell".
It does not implement virtual memory, swapping, copy-on-write, NUMA or
fragmentation control.

## 7.1 Sizing

```
4 GiB / 4 KiB = 1,048,576 frames
1,048,576 / 8 = 131,072 bytes = 128 KiB per bitmap
```

```c
#define PAGE_SIZE 4096u
#define MAX_PAGES (1u << 20) /* 4 GiB / 4 KiB */
#define PAGE_BITMAP_BYTES (MAX_PAGES / 8u)

static uint8_t page_bitmap[PAGE_BITMAP_BYTES];
static uint8_t alloc_bitmap[PAGE_BITMAP_BYTES];
```

Each bitmap is 128 KiB; together they add 256 KiB to `.bss`. On the current
build they sit at `0x001089E0` (`alloc_bitmap`) and `0x001289E0`
(`page_bitmap`), well below `__bss_end = 0x0014B0A0`. Because the kernel
reserves `[1 MiB, __bss_end)`, the bitmaps are automatically protected from
allocation and are identity-mapped by paging.

Two bitmaps are needed because "used" and "freeable" are different questions:

* `page_bitmap` answers "is this frame available?".
* `alloc_bitmap` answers "did this frame come from `pmm_alloc_page`?".

Reserved memory (kernel, Multiboot data, low 1 MiB) has `page_bitmap=1` but
`alloc_bitmap=0`, so the shell cannot free it. Page tables allocated with
`pmm_alloc_kernel_page` are also `page_bitmap=1`, `alloc_bitmap=0`, so they
stay reserved forever.

## 7.2 Bit operations

```c
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
```

Frame `n` lives in byte `n / 8` at bit `n % 8`. The shifts implement the
division and modulo: `page >> 3` and `page & 7`. Bit 0 is the least significant
bit of the byte, so frame 0 is bit 0 of `page_bitmap[0]`.

Convenience wrappers make the code read better:

```c
static inline int page_used(uint32_t page)      { return bit_test(page_bitmap, page); }
static inline void page_mark_used(uint32_t page){ bit_set(page_bitmap, page); }
static inline void page_mark_free(uint32_t page){ bit_clear(page_bitmap, page); }
```

## 7.3 Range helpers

```c
static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static uint64_t align_down(uint64_t value, uint64_t alignment)
{
    return value & ~(alignment - 1);
}
```

Both use 64-bit arithmetic because Multiboot reports 64-bit addresses. The
kernel runs on a 32-bit CPU, but doing the arithmetic in `uint64_t` avoids
overflow when `addr + len` crosses the 4 GiB boundary (which is then clamped by
`total_pages`). `alignment` is always 4096, a power of two, so the mask works.

```c
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
```

The alignments are deliberately conservative:

* `reserve_range` rounds the start **down** and the end **up**, so a partially
  covered frame is fully reserved.
* `free_range` rounds the start **up** and the end **down**, so a partially
  covered frame is not freed.

Together this guarantees that the kernel never frees a frame that shares a page
with reserved data. It can waste up to one frame at each end of a range, which
is negligible.

`>> 12` converts bytes to frame numbers (`/ 4096`). The shift is valid because
`PAGE_SIZE` is a compile-time power of two.

## 7.4 Discovering how much RAM exists

```c
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
```

Step by step:

1. Start with `highest = 0`.
2. If the memory map is present, walk it. Each entry has a `size` field giving
   the number of bytes **after** the size field, so the next entry is at
   `p + entry->size + sizeof(entry->size)`.
3. Only entries with `type == 1` (available RAM) matter. The highest usable
   top is `addr + len`.
4. If no mmap was found (or no available entries), fall back to the older
   `mem_upper` field, which reports KiB above 1 MiB. Adding 1024 KiB accounts
   for the first MiB: `(mem_upper + 1024) * 1024` bytes.
5. Round down to a whole frame and cap at 1,048,576 frames (4 GiB).

`total_pages` is therefore the **highest** available address, not the sum of
available RAM. Frames in holes below that top are initially marked used and
only freed if an available range covers them. This is the right model for a
flat physical allocator: a frame's availability is determined by the mmap, not
by arithmetic on totals.

If `mbi == 0` (invalid Multiboot magic) or no memory information exists,
`total_pages` ends up 0 and `pmm_init` substitutes `MAX_PAGES`. That is a
dangerous fallback: it claims 4 GiB of frames exist, but no ranges are freed,
so every frame stays marked used and `pmm_alloc_page` returns 0 immediately.
Paging then fails to allocate page tables and stays disabled. The kernel still
boots and prints diagnostics, but the memory commands show zero free pages.
See `11-end-to-end.md` for the boot behaviour with a bad magic.

## 7.5 Freeing the available ranges

```c
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
```

This is the same walk as `detect_total_pages`, but it clears bits instead of
tracking the maximum. A typical map contains entries such as:

| Range | Type | Meaning |
|---|---|---|
| `0x00000000`–`0x0009FC00` | available | conventional memory |
| `0x0009FC00`–`0x000A0000` | reserved | EBDA |
| `0x000F0000`–`0x00100000` | reserved | BIOS ROM |
| `0x00100000`–`0x07FE0000` | available | main RAM (example) |
| `0x07FE0000`–`0x08000000` | reserved | ACPI / firmware |

Only the two available ranges are freed. The reserved holes stay marked used.
The main RAM range includes the kernel image, which is reserved in the next
phase.

## 7.6 Initialization order

```c
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
```

Step by step:

1. **Determine the frame count.** `detect_total_pages` as above. If it returns
   zero, pretend the full 4 GiB range exists (but all of it remains used
   because no ranges are freed).
2. **Mark everything used.** `memset(page_bitmap, 0xFF, ...)` is the safe
   default: any frame not explicitly reported available, and any MMIO hole,
   cannot be handed out accidentally.
3. **Clear the freeable map.** `alloc_bitmap` starts all zero: nothing has been
   handed out yet.
4. **Free the available ranges.** This may free low memory, the kernel image
   and the Multiboot structures; the next steps re-reserve them.
5. **Reserve low 1 MiB.** `LOW_MEMORY_END = 0x00100000`. This covers the IVT,
   BDA, EBDA, BIOS ROM, VGA memory at `0xB8000` and the real-mode area. Even
   though some of it may be reported available, the kernel must not use it.
6. **Reserve the kernel image.** `KERNEL_BASE = 0x00100000` through
   `kernel_end = __bss_end`. This includes `.text`, `.rodata`, `.bss`, the
   boot stack, both PMM bitmaps, the IDT, the GDT, the page directory and every
   other static object. Rounding in `reserve_range` also reserves the partial
   page containing `__bss_end`.
7. **Reserve the Multiboot info structure.** Only `sizeof(*mbi)` bytes, not the
   extended structure. The struct is 52 bytes in this definition.
8. **Reserve the memory map.** `mmap_addr`..`mmap_addr + mmap_length`.
9. **Reserve the command line.** If present, reserve the string including its
   NUL terminator. `strlen` trusts the bootloader to provide a terminated
   string.
10. **Count free frames.** A full linear scan over `total_pages` recomputes
    `free_pages` after all the free/reserve operations. This is simpler and
    less error-prone than incrementing/decrementing a counter throughout.

### Why free first and reserve afterwards?

The Multiboot mmap is the authoritative list of usable RAM. Marking
"everything used" and then freeing only available entries would leave the
kernel's own memory free. Rather than special-case the kernel during the walk,
the code frees all available ranges and then carves out the reserved regions.
The order also makes overlapping reservations idempotent: `reserve_range` sets
bits, and setting an already-set bit is harmless.

## 7.7 Allocation

```c
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

void *pmm_alloc_page(void)        { return alloc_page(1); }
void *pmm_alloc_kernel_page(void){ return alloc_page(0); }
```

Step by step:

1. Scan from frame 0 upward. Low memory and the kernel are already marked
   used, so the first free frame is normally just above `__bss_end`.
2. On the first zero bit: set it (used), set or clear the freeable bit, and
   decrement `free_pages`.
3. Return the **physical** address as a pointer. Because paging is identity
   mapped, this pointer is also a valid virtual address after `paging_init`.
4. If no frame is free, return `0`. Frame 0 is always reserved, so null is a
   safe error sentinel.

Properties:

* **First fit.** The allocator always returns the lowest free frame, so memory
  is consumed roughly in address order. There is no free list, no per-size
  bucket and no best-fit search.
* **`pmm_alloc_page`** marks the frame freeable. The shell's `alloc`/`free`
  commands use it.
* **`pmm_alloc_kernel_page`** does **not** mark the frame freeable. Page tables
  use it, so `free` cannot release them.
* **No zeroing.** The returned frame contains whatever was there before
  (bootloader data, old kernel data, garbage). Callers that need zeroed memory
  must clear it themselves. `vmtest` writes before reading; page-table
  allocation explicitly zeroes each table.
* **No locking.** The scan and bit updates are not atomic. The shell is the
  only caller after boot, and interrupt handlers never allocate, so this is
  safe today. A future IRQ-time allocator would need a lock or interrupt
  disabling.

## 7.8 Freeing

```c
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
```

Step by step:

1. Reject null.
2. Reject unaligned addresses. Freeing a pointer into the middle of a frame
   would corrupt the bitmap's accounting.
3. Reject frames below 1 MiB. This is redundant with the `alloc_bitmap` check
   for normal operation (low memory is never in `alloc_bitmap`), but it makes
   the intent explicit and guards against a corrupted bitmap.
4. Reject frames at or above `total_pages`.
5. Reject frames whose `alloc_bitmap` bit is clear. This is the key safety
   check:
   * kernel image and boot data: used, not freeable;
   * low memory: used, not freeable;
   * page tables from `pmm_alloc_kernel_page`: used, not freeable;
   * a frame already freed once: bit cleared, so a double free is rejected;
   * a random aligned address: bit clear, rejected.
6. Clear both bits and increment `free_pages`.

The function returns 1 on success and 0 on failure. It does not check whether
the frame is still mapped by paging; unmapping is the caller's job. In
`vmtest`, the order is unmap then free, which is correct. Freeing a mapped
frame would leave a dangling virtual mapping.

## 7.9 Statistics

```c
uint32_t pmm_total_pages(void) { return total_pages; }
uint32_t pmm_free_pages(void)  { return free_pages; }
```

`total_pages` is the highest usable frame count, not the amount of installed
RAM. `free_pages` is maintained incrementally after initialization. The shell
computes `used = total - free` and converts to MiB by dividing by 256
(`1024 * 1024 / 4096`).

## 7.10 A worked example

Suppose the firmware reports 128 MiB with a simplified map:

* available: `0x00000000`–`0x0009FC00` (639 KiB)
* available: `0x00100000`–`0x08000000` (127 MiB)
* reserved holes: `0x0009FC00`–`0x00100000`

Then:

| Step | Calculation | Result |
|---|---|---|
| total frames | `0x08000000 / 0x1000` | 32,768 |
| free low range | `0x00000000`–`0x0009F000` | 159 frames |
| free high range | `0x00100000`–`0x08000000` | 32,512 frames |
| reserve low 1 MiB | frames 0–255 | −159 free (the rest were already used) |
| reserve kernel | `0x00100000`–`0x0014C000` | −76 frames |
| free after PMM init | | 32,436 frames |
| page tables | 32 tables for 32,768 frames | −32 frames |
| free after paging | | 32,404 frames |
| identity-mapped | | 32,768 pages = 128 MiB |

Real QEMU maps usually end slightly below `0x08000000` because the last few
MiB are reserved for ACPI/firmware. On this machine the available high range
ends at `0x07FE0000`, so there are 32,736 frames (32 fewer). Multiboot also
places the command line at `0x0014C000`, one frame above `__bss_end`; `pmm_init`
reserves it. Page tables then start at `0x0014D000`. The live result is
**32,371 free / 365 used** frames, and the first allocatable frame is
`0x0016D000`. The breakdown is 256 low-memory frames + 76 kernel frames +
1 command-line frame + 32 page tables = 365.

The simplified example above gives 364 used frames because it omits the
command-line page. In both cases, only 76 frames belong to the kernel image and
32 to page tables; the rest are low memory, the command line and reserved
holes below the top of RAM. A frame count is not a measure of kernel size.

## 7.11 Invariants

These should always hold after `pmm_init`:

1. Every frame `< 256` has `page_bitmap = 1`.
2. Every frame in `[1 MiB, __bss_end)` has `page_bitmap = 1`.
3. `alloc_bitmap` has a bit set only if the corresponding `page_bitmap` bit is
   set. (`pmm_free_page` clears both; reserved frames have both clear.)
4. `free_pages` equals the number of zero bits in `[0, total_pages)`.
5. No frame returned by `pmm_alloc_page`/`pmm_alloc_kernel_page` is currently
   marked free.
6. Frame 0 is never returned, so `0` is an unambiguous OOM result.
7. Every returned address is 4 KiB aligned and below `total_pages * 4096`.

`pmm_free_page` preserves all of them. A bug that sets a bit without updating
`free_pages` would make the statistics drift, but the allocator itself would
still be safe.

## 7.12 Limitations and sharp edges

* **No zeroing.** `pmm_alloc_page` returns dirty memory.
* **No locking / no IRQ safety.** Allocation and freeing must not race.
* **O(n) first-fit.** The scan restarts from frame 0 every time. With a nearly
  full 4 GiB bitmap this is up to a million bit tests per allocation. A free
  list or a "next free" hint would be faster.
* **No coalescing / no buddy system.** Bitmaps do not care about adjacency, so
  fragmentation is not tracked or repaired. It does not matter for 4 KiB
  frames with no larger allocations, but it would matter if contiguous
  multi-frame allocations were added.
* **32-bit physical only.** Frames above 4 GiB are not represented. PAE and
  long mode are out of scope.
* **No module reservation.** `mods_count`/`mods_addr` are ignored, so a GRUB
  module loaded into available RAM could be overwritten. bnOS loads none.
* **Extended Multiboot data is not reserved.** Only the first 52 bytes of the
  info struct, the mmap and the cmdline are protected. Drive, ROM and
  framebuffer tables are not used, and the struct is usually in low memory.
* **Malformed mmap entries are trusted.** If `entry->size` were 0, the loop
  would advance by only 4 bytes per iteration (`entry->size +
  sizeof(entry->size)`), reading misaligned garbage until `end`; it would still
  terminate. A correct bootloader always reports size 20. There is no
  validation of `addr + len` overflow beyond the 64-bit arithmetic and
  clamping.
* **`mem_lower` is ignored.** Only `mem_upper` is used as a fallback.
* **No memory map sorting.** Entries are processed in the order provided;
  free/reserve is order-independent because it only sets/clears bits.
* **`free_pages` can be wrong if the MMIO holes above `total_pages` are
  considered.** They are outside the managed range by construction.
* **No page poisoning or use-after-free detection.** A freed frame can still
  be read or written through a stale pointer; the bitmap does not revoke
  mappings. `vmtest` unmaps before freeing to avoid this.

## 7.13 Interaction with paging

`paging_init` calls `pmm_total_pages()` to decide how much to identity map and
`pmm_alloc_kernel_page()` for each page table. This creates a deliberate
ordering dependency:

1. `pmm_init` must run first, or there are no free frames and no known limit.
2. `paging_init` allocates page tables from the PMM. Those frames are marked
   used and permanently reserved.
3. The allocator's returned physical addresses remain valid as virtual
   addresses because every frame below `total_pages` is identity mapped.
4. Freeing a frame does not unmap it. The shell's `free` therefore leaves the
   identity mapping in place; the frame can be reused, but nothing should
   access it through the old pointer.

Next: [08-paging.md](08-paging.md).
