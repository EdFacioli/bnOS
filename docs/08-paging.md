# 8. Paging

Files: `src/paging.c`, `src/paging.h`.

bnOS uses legacy 32-bit paging (no PAE) with 4 KiB pages and two levels: a page
directory and page tables. It builds an identity map for every physical frame
the PMM manages, then adds `paging_map_page`/`paging_unmap_page` for arbitrary
virtual mappings. Paging is enabled only after the PMM and before the shell
starts.

## 8.1 The 32-bit paging model

A 32-bit virtual address is split into three fields:

```
 31              22 21              12 11                 0
+------------------+------------------+--------------------+
|  directory index |   table index    |   page offset      |
|     10 bits      |     10 bits      |      12 bits       |
+------------------+------------------+--------------------+
```

Translation:

```
 CR3 (physical)                PDE (4 KiB)              PTE (4 KiB)
 +-----------+     +-----------+     +-----------+
 | dir base  | --> | entry 768 | --> | entry 0   | --> physical frame
 +-----------+     +-----------+     +-----------+
       |                 |                 |
   page directory    page table        page table
   (1024 PDEs)       (1024 PTEs)       (1024 PTEs)
```

* **CR3** holds the physical address of the page directory. It must be 4 KiB
  aligned; the low 12 bits are flags.
* A **PDE** (page-directory entry) either points to a page table (4 KiB
  paging) or, with `PS=1`, maps a 4 MiB page (not used here).
* A **PTE** (page-table entry) points to a 4 KiB physical frame plus flags.
* The page offset (low 12 bits) is copied unchanged.

Each page table covers 4 MiB (`1024 × 4 KiB`); the directory covers 4 GiB
(`1024 × 4 MiB`).

## 8.2 Entry bits used

`paging.c` defines only the bits it needs:

```c
#define PTE_PRESENT 0x001u
#define PTE_WRITE   0x002u
#define PDE_PRESENT 0x001u
#define PDE_WRITE   0x002u
#define CR0_PG      0x80000000u
```

| Bit | Name | Meaning here |
|---|---|---|
| 0 | P | present; if 0, an access raises a page fault |
| 1 | R/W | 1 = writable, 0 = read-only (ring 0 can write to read-only pages? No: CR0.WP is 0 by default on i386, so ring 0 can write read-only pages. The kernel never sets CR0.WP.) |
| 2 | U/S | 0 = supervisor only (ring 0). All mappings are supervisor. |
| 3 | PWT | 0 = write-back caching |
| 4 | PCD | 0 = cacheable |
| 5 | A | accessed; the CPU sets it |
| 6 | D | dirty (PTE only); the CPU sets it |
| 7 | PS | 0 = 4 KiB page table (PDE); page size (PTE, reserved) |
| 8 | G | 0 = not global |
| 9–11 | AVL | 0 = unused by hardware |
| 12–31 | address | physical frame or page table base |

Every mapping created by `paging_init` and `paging_map_page` is
`P | RW`, supervisor, cacheable, 4 KiB. There is no user bit, no NX (that
requires PAE/long mode), no global bit and no cache-control policy.

## 8.3 The page directory

```c
static uint32_t page_directory[1024] __attribute__((aligned(PAGE_SIZE)));
static uint32_t mapped_pages;
```

The array is 4 KiB and must be page aligned, hence the attribute. It lives in
`.bss` (current address `0x0014A000`). Its physical address is what goes into
CR3. `mapped_pages` counts present PTEs, not page tables.

## 8.4 Building the identity map

```c
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
```

### Step 1: decide how much to map

* `limit = pmm_total_pages()` is the count of frames the PMM manages. Mapping
  every one of them keeps all PMM-returned pointers valid.
* `minimum = ceil(kernel_end / 4096)` covers the kernel image itself.
* `minimum` is raised to at least 256 pages (1 MiB) so the VGA buffer at
  `0xB8000` and low-memory data remain reachable. `pmm_total_pages()` is
  normally much larger, but this handles tiny-memory configurations.
* If the PMM knows less memory than the kernel image (should not happen), map
  at least the kernel.

### Step 2: allocate page tables

`tables = ceil(limit / 1024)`. Each table maps 1024 frames (4 MiB). With 128 MiB
of managed RAM, `limit = 32768`, so `tables = 32`.

`pmm_alloc_kernel_page()` returns a physical frame and marks it permanently
reserved. Paging is still off, so the returned pointer is directly usable and
the page table can be written. The page table is zeroed entry by entry below
(not by `memset`, but by the loop's `else` branch).

If allocation fails, the function prints an error and returns **without
enabling paging**. The loader's paging-off state remains. The partially built
tables stay allocated and mapped only in the (unused) directory. This is a
graceful degradation: the shell still runs in flat physical mode, but
`paging` reports disabled and `vmtest` is meaningless.

### Step 3: fill each table

For every entry:

* `page = index * 1024 + entry` is the linear page number.
* If `page < limit`, write `page * 4096 | P | RW`. Because the page number is
  also the physical frame, this is an identity mapping: virtual address
  `page * 4096` maps to physical address `page * 4096`.
* Otherwise write 0 (not present), so accesses beyond `limit` fault.

`mapped_pages` is incremented for each present PTE. It ends up equal to
`limit`.

### Step 4: fill the page directory

For each table, store its physical address (`table` at this point is physical
because paging is off) with `P | RW`. This sets PDE `index` for the 4 MiB
region `[index*4 MiB, (index+1)*4 MiB)`.

PDEs from `tables` to 1023 are cleared. The array was already zeroed by
`_start`, but clearing again is cheap and makes the function self-contained if
it is ever called twice.

### Step 5: load CR3

```c
__asm__ volatile("mov %0, %%cr3" : : "r"(...page_directory) : "memory");
```

`CR3` receives the physical address of the page directory. At this moment
paging is still off, so the CPU does not use it yet. Writing CR3 also flushes
the TLB (architecturally, a MOV to CR3 invalidates non-global TLB entries).

### Step 6: set CR0.PG

```c
uint32_t cr0;
__asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
cr0 |= CR0_PG;
__asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
```

Read-modify-write preserves all other control bits (PE, MP, EM, TS, NE, WP,
AM, NW, CD). Setting bit 31 enables paging. The very next instruction fetch
uses the new mapping. It succeeds because the identity map covers the kernel's
current `EIP`, the stack, the GDT/IDT and all data. No jump or segment reload
is required.

The `"memory"` clobbers prevent the compiler from reordering memory accesses
across the control-register writes.

### Why identity mapping?

There is no virtual allocator and no higher-half kernel. Every pointer in the
kernel is a physical address:

* the boot stack and `.bss`;
* the Multiboot info and mmap;
* VGA memory at `0xB8000`;
* PMM-returned frames;
* page tables and the page directory.

An identity map makes all of them valid virtual addresses with zero pointer
translation. The kernel can enable paging without relinking, without a
higher-half offset, and without fixing up pointers. `vmtest` then demonstrates
a non-identity mapping on top of this baseline.

## 8.5 Finding a page table

```c
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
```

* `virt >> 22` extracts the top 10 bits (directory index).
* If the PDE is not present and `create == 0`, return `0` ("no table").
* If creation is requested, allocate a kernel page, zero all 1024 PTEs, and
  install the PDE with `P | RW`.
* Return the table address by masking off the flag bits (`& 0xFFFFF000`).

Because paging is on when this runs (except during `paging_init`, which does
not call it), the page table's physical address is also its virtual address:
the table frame is below `limit` and therefore identity mapped. This is a
subtle but essential invariant. A page table allocated from a frame that was
not identity mapped could not be dereferenced as a pointer.

The function does not flush the TLB itself. `paging_map_page` flushes the
specific virtual page after installing the PTE, which makes the new mapping
visible even when a PDE was just created. If a caller used `page_table_for`
directly and only changed the PDE, it would need its own TLB invalidation.

## 8.6 Querying a mapping

```c
int paging_is_mapped(uint32_t virt)
{
    uint32_t *table = page_table_for(virt, 0);

    if (table == 0)
        return 0;
    return (table[(virt >> 12) & 0x3FF] & PTE_PRESENT) != 0;
}
```

`(virt >> 12) & 0x3FF` extracts the middle 10 bits (table index). The function
returns true only if both the PDE and the PTE are present. It does not check
whether the mapping points to real RAM.

## 8.7 Mapping a page

```c
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
```

Step by step:

1. Both `virt` and `phys` must be 4 KiB aligned. The low 12 bits are the page
   offset and cannot be encoded in a PTE.
2. Get or create the page table for `virt`.
3. Locate the PTE.
4. If the PTE was not present, increment `mapped_pages`; replacing an existing
   mapping does not change the count.
5. Store the new PTE: physical base + present, plus writable if requested.
   The assignment **replaces** all previous flags: user, accessed, dirty,
   global and cache bits are lost. That is fine here because all mappings are
   supervisor and cacheable.
6. `invlpg` invalidates the TLB entry for that virtual address. Without it, the
   CPU could keep using an old translation. `invlpg` takes a virtual address
   (in memory operand form), not a physical one.

The function does not:

* verify that `phys` is a managed or available frame;
* check whether `virt` already maps a different frame (it silently remaps and
  leaks the old frame to the caller's accounting);
* free the old frame;
* set the user bit, so ring 3 could not access it;
* handle 4 MiB pages;
* map more than one page.

`writable == 0` creates a read-only supervisor mapping. Note that CR0.WP is 0,
so ring 0 can still write to it; read-only matters only if user mode and WP are
enabled later.

## 8.8 Unmapping a page

```c
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
```

* Alignment is checked.
* The table must already exist; `create=0` means no allocation on unmap.
* A not-present PTE is an error (returns 0), not a silent success.
* The PTE is zeroed, the count decremented, the TLB entry invalidated.
* The physical frame is **not** freed. The caller decides whether to call
  `pmm_free_page` (as `vmtest` does).
* The page table is **not** freed, even if every PTE becomes zero. There is no
  page-table reclamation.

## 8.9 The TLB

The translation lookaside buffer caches virtual→physical translations. After
changing a PTE or PDE, the CPU may keep using the old entry.

* `invlpg (virt)` invalidates one page. It is used after every map and unmap.
* A write to CR3 invalidates all non-global entries (and the current build
  flushes everything). `paging_init` uses this.
* There is no need for TLB shootdown IPIs: the kernel is single-CPU and
  non-preemptive.
* Mapping a page that was already mapped with the same physical address still
  needs `invlpg` if the flags changed; the code always invalidates.

## 8.10 `vmtest`: a non-identity mapping

The shell command:

```c
#define VMTEST_ADDRESS 0xC0000000u

static void cmd_vmtest(int argc, char **argv)
{
    if (paging_is_mapped(VMTEST_ADDRESS)) { ... skip ... }

    void *frame = pmm_alloc_page();
    if (frame == 0) { ... OOM ... }

    uint32_t phys = (uint32_t)(uintptr_t)frame;
    if (!paging_map_page(VMTEST_ADDRESS, phys, 1)) { ... }

    volatile uint32_t *mapped = (volatile uint32_t *)(uintptr_t)VMTEST_ADDRESS;
    *mapped = 0xDEADBEEFu;
    uint32_t readback = *mapped;

    paging_unmap_page(VMTEST_ADDRESS);
    pmm_free_page(frame);

    vga_printf("vmtest: phys 0x%X -> virt 0x%X, ...", ...);
}
```

Why `0xC0000000`?

* Top 10 bits: `0xC0000000 >> 22 = 768` (`0x300`). The kernel's identity map
  normally uses only `tables` PDEs (32 for 128 MiB), so index 768 is free.
* Middle 10 bits: `(0xC0000000 >> 12) & 0x3FF = 0`.
* Offset: 0.
* The address is canonical and easy to recognize.

Step by step:

1. If something is already mapped there, skip. This happens on machines with
   ≥ 3 GiB of managed RAM, because the identity map covers `0xC0000000`.
2. Allocate a 4 KiB frame from the PMM. This frame is somewhere above the
   kernel, not at 3 GiB.
3. Map `0xC0000000` → `phys` with write permission. `page_table_for` creates a
   new page table (one physical page, permanently reserved) for PDE 768, and
   the PTE is installed.
4. Write `0xDEADBEEF` through the virtual address. This goes through the TLB,
   page table and physical frame.
5. Read it back. `volatile` prevents the compiler from folding the read into a
   constant or eliminating the store.
6. Unmap `0xC0000000` (PTE zero, `invlpg`).
7. Free the physical frame with `pmm_free_page`.
8. Print the physical/virtual pair and the readback.

The page table created for PDE 768 is never freed. The first `vmtest` leaks one
4 KiB page permanently; subsequent runs reuse the existing table. This is
intentional in the sense that `pmm_alloc_kernel_page` marks it unfreeable, but
it is still a leak in the abstract.

If paging is disabled while the PMM still has free frames, the write goes
directly to physical `0xC0000000`. On a machine with less than 3 GiB that
address may not exist, and the result is undefined. The command does not check
`paging_enabled()`. In the normal failure path, however, `paging_init` runs out
of frames while allocating page tables, so the PMM has no free frames either
and `vmtest` stops earlier with "out of physical memory".

## 8.11 Page faults

Vector 14 is installed like every other exception. When a translation fails,
the CPU pushes an error code and sets `CR2` to the faulting linear address.
`isr_dispatch` prints:

```
EXCEPTION 14: page fault (error=0x...)
  fault address : 0x...
  cause         : page not present, read, kernel mode
System halted.
```

Error-code bits:

| Bit | Mask | 0 | 1 |
|---|---|---|---|
| 0 | P | page not present | protection violation |
| 1 | W/R | read | write |
| 2 | U/S | kernel mode | user mode |
| 3 | RSVD | no reserved bit | reserved bit set in a paging structure |
| 4 | I/D | data access | instruction fetch (newer CPUs) |

A read of an unmapped address (e.g. `0xC0000000` before `vmtest`) produces
`error = 0x0`: not present, read, kernel. Writing to a read-only mapping with
CR0.WP=1 would produce `P=1, W/R=1`. bnOS never sets WP and never creates user
mappings, so the protection-violation cases are mostly theoretical.

There is no fault recovery, no demand paging, no stack growth and no
copy-on-write. Any page fault halts the machine.

## 8.12 What the identity map covers

For `limit` frames, PTEs exist for pages `0 .. limit-1`. That includes:

* low memory (`0`–`1 MiB`), even though the PMM reserves it;
* the kernel image, stack, bitmaps, IDT, GDT, page directory;
* all free and allocated frames below the top of usable RAM;
* MMIO holes below the top (mapped as normal cacheable RAM — potentially
  wrong for device memory, but the kernel does not touch them).

It does **not** cover:

* addresses at or above `limit * 4096` (unless `vmtest` adds one);
* physical RAM above 4 GiB (not represented);
* a non-identity virtual address space (there is none).

A consequence: virtual address 0 is mapped and writable. A null-pointer
dereference will **not** fault; it will read or write low physical memory. This
is a common simplification in identity-mapped kernels, but it hides bugs.

## 8.13 Limitations

* **No virtual allocator.** `paging_map_page` maps a caller-supplied physical
  frame; there is no `malloc`-like virtual region manager, no `mmap`, no
  `brk`.
* **No demand paging.** All mappings are present from the start; there is no
  "allocate on first touch".
* **No swapping.** No disk, no page cache, no eviction.
* **No copy-on-write or fork.** No processes at all.
* **No user mode.** All pages are supervisor; there is no U/S=1 mapping.
* **No NX.** Requires PAE or long mode; all mapped pages are executable.
* **No write protection.** CR0.WP=0 and all PTEs are writable, so even
  `.text`/`.rodata` can be overwritten.
* **No 4 MiB pages.** `PS` is never set, so every 4 MiB region needs a page
  table (32 tables for 128 MiB).
* **No page-table reclamation.** Unmapping all PTEs in a table leaves the
  table allocated.
* **No cache policy control.** PWT/PCD are 0; device MMIO would need
  uncacheable mappings.
* **No TLB shootdown.** Single CPU, no concurrency.
* **No guard pages.** Stack overflow corrupts adjacent `.bss` rather than
  faulting.
* **No self-test at boot.** `paging_init` does not verify that a known address
  reads back correctly; `vmtest` is manual.
* **Failure is non-fatal but silent-ish.** If a page table allocation fails,
  paging is never enabled; the boot banner reports `Paging: disabled`. The
  shell still works because everything runs in physical mode.

## 8.14 Worked address examples

| Virtual | PDE index | PTE index | Offset | Notes |
|---|---|---|---|---|
| `0x00100000` | 0 | 256 (`0x100`) | 0 | kernel start |
| `0x0014A000` | 0 | 330 (`0x14A`) | 0 | page directory |
| `0x000B8000` | 0 | 184 (`0xB8`) | 0 | VGA text buffer |
| `0x0014B0A0` | 0 | 331 (`0x14B`) | 0xA0 | `__bss_end` |
| `0xC0000000` | 768 (`0x300`) | 0 | 0 | `vmtest` |
| `0xC0001000` | 768 | 1 | 0 | next page in the same table |

The kernel image, VGA buffer, page directory and both PMM bitmaps all live in
the first page table (PDE 0), which maps `0x00000000`–`0x003FFFFF`. That table
is created first and is essential for the kernel to keep running after
`CR0.PG` is set.

Next: [09-shell.md](09-shell.md).
