# 2. Global Descriptor Table

Files: `src/gdt.c`, `src/gdt.h`, `src/gdt_flush.S`.

Even in 32-bit protected mode, every memory access goes through a segment
descriptor. The kernel installs the smallest useful GDT: a null descriptor, a
flat ring-0 code segment and a flat ring-0 data segment. There is no TSS, no
user mode and no per-CPU descriptor.

## 2.1 What a descriptor contains

An x86 segment descriptor is eight bytes:

```
 63          56 55 54 53 52 51       48 47    40 39       32
 +--------------+--+--+--+--+----------+--------+-----------+
 |  base 31:24  |G |D |L |A | limit    | access | base 23:16|
 |              |  |B |  |V | 19:16    |        |           |
 +--------------+--+--+--+--+----------+--------+-----------+
 31                      16 15                         0
 +-------------------------+---------------------------+
 |       base 15:0         |       limit 15:0          |
 +-------------------------+---------------------------+
```

* `base` is split across three fields (low 16, middle 8, high 8).
* `limit` is split into low 16 bits and high 4 bits.
* `access` controls present, privilege, type and direction.
* `granularity` contains `G` (limit unit), `D/B` (default operand/stack size),
  `L` (long mode, must be 0 here), `AVL` (available for software) and the high
  nibble of the limit.

`gdt.c` mirrors this layout with a packed struct:

```c
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));
```

`__attribute__((packed))` is essential: without it the compiler could insert
padding and every field would be at the wrong offset.

The pointer passed to `lgdt` is the 6-byte "pseudo-descriptor":

```c
struct gdt_ptr {
    uint16_t limit;  /* size in bytes minus one */
    uint32_t base;   /* linear address of gdt[0] */
} __attribute__((packed));
```

## 2.2 The three entries

```c
enum {
    GDT_ENTRY_NULL = 0,
    GDT_ENTRY_CODE = 1,
    GDT_ENTRY_DATA = 2,
    GDT_ENTRIES = 3,
};

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr gdt_descriptor;
```

### Entry 0: null

```c
gdt_set_entry(GDT_ENTRY_NULL, 0, 0, 0x00, 0x00);
```

The CPU never loads selector 0; using it raises #GP. The null descriptor is
required by the architecture and is a useful "no segment" value.

### Entry 1: kernel code

```c
/* 0x9A: present, ring 0, code, executable/readable. */
gdt_set_entry(GDT_ENTRY_CODE, 0, 0xFFFFF, 0x9A, 0xCF);
```

Decoding `access = 0x9A = 1001_1010b`:

| Bits | Field | Value | Meaning |
|---|---|---|---|
| 7 | P | 1 | present |
| 6–5 | DPL | 00 | ring 0 |
| 4 | S | 1 | code/data (not system) |
| 3 | E | 1 | executable |
| 2 | DC | 0 | non-conforming |
| 1 | RW | 1 | readable |
| 0 | A | 0 | accessed (CPU sets it) |

Decoding `granularity = 0xCF = 1100_1111b`:

| Bits | Field | Value | Meaning |
|---|---|---|---|
| 7 | G | 1 | limit is in 4 KiB units |
| 6 | D/B | 1 | 32-bit default operand size and 32-bit stack |
| 5 | L | 0 | not long mode |
| 4 | AVL | 0 | unused |
| 3–0 | limit 19:16 | 0xF | high nibble of the limit |

With `base = 0` and `limit = 0xFFFFF` scaled by 4 KiB, the segment spans
`0x00000000`..`0xFFFFFFFF` (4 GiB). That is the flat model: linear address
equals offset.

### Entry 2: kernel data

```c
/* 0x92: present, ring 0, data, read/write. */
gdt_set_entry(GDT_ENTRY_DATA, 0, 0xFFFFF, 0x92, 0xCF);
```

`access = 0x92 = 1001_0010b`: present, ring 0, data, expand-up, writable.
The granularity is identical, so the data segment also covers all 4 GiB.

## 2.3 Encoding a descriptor

```c
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
```

The function splits the 32-bit base into the three hardware fields and the
20-bit limit into its low and high parts. `granularity & 0xF0` masks out the
low nibble so the caller cannot accidentally overwrite the limit's high nibble.
The low nibble is taken from `limit >> 16`, which for `0xFFFFF` is `0xF`.

Note that `limit` is the raw 20-bit value, not the byte size. Passing
`0xFFFFF` plus `G=1` produces a byte limit of `0xFFFFF * 4096 + 0xFFF`, i.e.
4 GiB minus one byte.

## 2.4 Installing the table

```c
void gdt_init(void)
{
    gdt_set_entry(GDT_ENTRY_NULL, 0, 0, 0x00, 0x00);
    gdt_set_entry(GDT_ENTRY_CODE, 0, 0xFFFFF, 0x9A, 0xCF);
    gdt_set_entry(GDT_ENTRY_DATA, 0, 0xFFFFF, 0x92, 0xCF);

    gdt_descriptor.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_descriptor.base = (uint32_t)(uintptr_t)&gdt;

    gdt_flush((uint32_t)(uintptr_t)&gdt_descriptor);
}
```

`sizeof(gdt) - 1` is 23 for three eight-byte entries, exactly the value
`lgdt` expects. `gdt_descriptor.base` is the linear (here physical) address of
the array. `gdt_flush` receives that address as its only argument.

## 2.5 `gdt_flush`: making the CPU use the new table

```asm
gdt_flush:
    mov 4(%esp), %eax
    lgdt (%eax)

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    ljmp $0x08, $1f
1:  ret
```

Step by step:

1. `mov 4(%esp), %eax` reads the first cdecl argument (the pointer to
   `gdt_descriptor`). At entry, `(%esp)` is the return address and `4(%esp)` is
   the argument.
2. `lgdt (%eax)` loads the 6-byte pseudo-descriptor into `GDTR`. It does not
   change any segment register.
3. `mov $0x10, %ax` selects index 2 (`0x10 >> 3 = 2`), table indicator 0 (GDT)
   and RPL 0: the kernel data descriptor.
4. `ds`, `es`, `fs`, `gs` and `ss` are reloaded with `0x10`. Reloading `ss`
   with a writable flat segment is safe because interrupts are disabled and
   the stack remains at the same linear address. (On x86, `mov` to `ss`
   inhibits interrupts for one instruction anyway.)
5. `ljmp $0x08, $1f` loads `CS` with selector `0x08` (index 1, GDT, RPL 0) and
   jumps to the local label `1:`. A far jump is mandatory: there is no way to
   load `CS` with a plain `mov`, and the CPU must be forced to re-fetch the
   instruction stream with the new code descriptor's cached information.
6. `ret` returns to `gdt_init`.

The far jump target `1f` is the instruction immediately after the jump, so the
control flow is unchanged; only `CS` and its cached base/limit change. The
`ret` then pops the return address that `gdt_init`'s `call` pushed.

## 2.6 Selector arithmetic

A selector is a 16-bit value:

```
 15                              3  2  1  0
+--------------------------------+--+-----+
|          index                 |TI| RPL |
+--------------------------------+--+-----+
```

| Selector | Index | Descriptor | RPL |
|---|---|---|---|
| `0x00` | 0 | null | 0 |
| `0x08` | 1 | kernel code | 0 |
| `0x10` | 2 | kernel data | 0 |

`RPL` (requested privilege level) and `DPL` (descriptor privilege level) are
both 0, so ring 0 can load either segment. There are no `0x18`/`0x20` user
selectors, so a future `iret` to ring 3 would fail.

## 2.7 Why a flat model?

With base 0 and limit 4 GiB, `segment:offset` addressing degenerates to
`offset`. The kernel can use ordinary pointers, the compiler does not need to
distinguish near/far pointers, and paging becomes the only address translation
layer. This is the standard choice for 32-bit kernels.

The alternative — non-zero bases or small limits — would require reloading
segments when switching address spaces and would complicate every pointer.
There is no benefit here because there is no user isolation.

## 2.8 What is missing

* **No TSS.** A Task State Segment is required for ring transitions
  (ring 3 → ring 0) and for `ltr`-based hardware task switching. bnOS never
  leaves ring 0, so no TSS is needed. Adding user mode would require a TSS and
  a `0x28`/`0x30` pair of user descriptors.
* **No per-CPU GDT.** There is one CPU and one table.
* **No TLS / `%gs` base.** `%gs` is a flat data selector; there is no
  `MSR_GS_BASE` setup (that is a 64-bit feature anyway).
* **No stack guard.** The boot stack is a plain 16 KiB region.
* **No descriptor for 16-bit code.** The kernel never returns to real mode.

## 2.9 Interaction with later subsystems

* `idt.c` hard-codes `KERNEL_CODE_SELECTOR 0x08` for every gate, so the GDT
  must be loaded before `idt_init`.
* `isr.S` loads `0x10` into `ds/es/fs/gs` on every interrupt, so the data
  descriptor must be valid before the first interrupt.
* Paging does not change descriptors: base 0 still means virtual address 0,
  and the identity map keeps the kernel reachable at its link addresses.
* The bootloader's GDT is discarded. Any selectors cached from it (including
  `CS`) are replaced by `gdt_flush`.

Next: [03-idt-and-interrupts.md](03-idt-and-interrupts.md).
