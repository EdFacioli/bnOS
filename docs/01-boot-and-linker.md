# 1. Build, linker script and boot

This document covers everything that happens before `kernel_main`: how the
image is compiled, how it is laid out in memory, how the bootloader finds it,
and what `src/boot.S` does instruction by instruction.

Files: `Makefile`, `linker.ld`, `src/boot.S`, `src/multiboot.h`.

## 1.1 What "freestanding" means here

The kernel is compiled with no C library and no runtime support. That has three
consequences visible throughout the tree:

1. The compiler may still emit calls to `memset`, `memcpy` and `memmove` when it
   lowers struct copies, array initialisation or large assignments. `src/lib.c`
   provides them.
2. There is no `crt0`, no `main`, no constructors and no `exit`. The entry point
   is the assembly label `_start`, declared in `linker.ld` with `ENTRY(_start)`.
3. There is no OS underneath, so every helper (`vga_printf`, `strtoul`,
   `strlen`) is implemented in the tree, and mistakes such as a page fault
   cannot be recovered by the host.

## 1.2 The Makefile, flag by flag

```make
CFLAGS  := -m32 -std=gnu11 -O2 -Wall -Wextra -Werror \
           -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
           -fno-builtin -fno-asynchronous-unwind-tables -mno-red-zone
ASFLAGS := -m32
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib -z noexecstack --build-id=none
```

| Flag | Why it is there |
|---|---|
| `-m32` | Emit 32-bit x86 code. The CPU starts in 32-bit protected mode, so the kernel must be i386 code, not x86-64. |
| `-std=gnu11` | C11 plus GNU extensions (designated initializers are used in the keymap). |
| `-O2` | Optimisation. This is why `volatile` matters in `vmtest` and the keyboard ring buffer. |
| `-Wall -Wextra -Werror` | Treat warnings as errors; keeps the freestanding code honest. |
| `-ffreestanding` | Do not assume a hosted implementation; `main` is not special, and the standard library is not available. |
| `-fno-pic -fno-pie` | Generate absolute-address code. There is no dynamic loader and no GOT; the kernel is linked at 1 MiB and runs there. |
| `-fno-stack-protector` | No `%gs`-based canary or `__stack_chk_fail`; the CPU does not have a TLS setup. |
| `-fno-builtin` | Do not replace loops with libc calls or builtins the kernel does not provide. |
| `-fno-asynchronous-unwind-tables` | No `.eh_frame`; there is no unwinder. |
| `-mno-red-zone` | No red zone below `%esp`. On i386 this is mostly a no-op (the red zone is an x86-64 concept), but it documents the intent. |
| `-nostdlib` (link) | Do not link `crt0.o`, `libc`, or the default startup files. |
| `-T linker.ld` | Use the kernel's own layout. |
| `-z noexecstack` | Mark the stack non-executable in the ELF program headers. |
| `--build-id=none` | No build-id note; it would be discarded anyway, but this keeps the link clean. |

The object list is explicit:

```make
OBJS := $(BUILD)/boot.o $(BUILD)/gdt_flush.o $(BUILD)/isr.o \
        $(BUILD)/kernel.o $(BUILD)/vga.o $(BUILD)/gdt.o $(BUILD)/idt.o \
        $(BUILD)/pic.o $(BUILD)/keyboard.o $(BUILD)/pmm.o $(BUILD)/paging.o \
        $(BUILD)/shell.o $(BUILD)/lib.o
```

Pattern rules compile `.c` and `.S` files into `build/`:

```make
$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@
```

`.S` (capital S) is preprocessed by the C preprocessor before assembly, which
is why `boot.S` and `isr.S` can use `#`-free assembly but still benefit from
GNU as macros. There is no automatic header dependency tracking (`-MMD`), so
editing a `.h` file does not force a rebuild; run `make clean` when changing
headers.

## 1.3 The linker script

```ld
ENTRY(_start)
OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)

SECTIONS
{
    . = 1M;

    .text : ALIGN(4K)
    {
        KEEP(*(.multiboot))
        *(.text .text.*)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(4K)
    {
        *(.data .data.*)
    }

    .bss : ALIGN(4K)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
```

Step by step:

1. `ENTRY(_start)` sets the ELF entry point. The bootloader jumps there, not to
   the first byte of the file.
2. `. = 1M` places the first section at physical/virtual `0x00100000`. Low
   memory below 1 MiB is left for the BIOS, the EBDA, the VGA buffer at
   `0xB8000` and the Multiboot structures.
3. `.text` starts with `KEEP(*(.multiboot))`. This guarantees the 12-byte
   Multiboot header is the first thing in the executable section. `KEEP`
   prevents `--gc-sections`-style removal (not used here, but correct).
4. Every section is `ALIGN(4K)`, so paging can map whole frames without
   splitting section boundaries.
5. `__bss_start` and `__bss_end` are absolute symbols around `.bss`. `boot.S`
   uses them to zero the region; `kernel.c` uses `__bss_end` as the end of the
   kernel image for PMM reservation.
6. `/DISCARD/` drops comment, unwind and note sections. `--build-id=none`
   avoids a note section that would otherwise be discarded.

There is no `.data` content in the current build, but the section exists for
future use. `.bss` is `NOBITS`: it occupies memory but no file bytes, and the
bootloader (or `_start`) must zero it.

### Actual layout of the current build

`readelf -S` and `nm` on this machine produce:

| Section / symbol | Address | Size | Notes |
|---|---|---|---|
| `.text` | `0x00100000` | `0x21EC` | Multiboot header + code |
| `.rodata` | `0x00103000` | `0x0D38` | keymaps, command table, stub table |
| `.bss` | `0x00104000` | `0x470A0` | stack, IDT, bitmaps, page directory |
| `__bss_start` | `0x00104000` | — | first byte to zero |
| `__bss_end` | `0x0014B0A0` | — | kernel end passed to the PMM |
| `stack_bottom` | `0x00104000` | 16 KiB | boot stack |
| `stack_top` | `0x00108000` | — | initial `%esp` |
| `page_bitmap` | `0x001289E0` | 128 KiB | PMM |
| `alloc_bitmap` | `0x001089E0` | 128 KiB | PMM |
| `page_directory` | `0x0014A000` | 4 KiB | must be 4 KiB aligned |
| `_start` | `0x0010000C` | 37 bytes | entry point |

The page directory is the last 4 KiB page before `__bss_end`; the kernel
reserves everything from `0x00100000` to `0x0014B0A0`, so the page directory,
both bitmaps, the IDT, the stack and all static data are automatically protected
from the allocator.

## 1.4 The Multiboot v1 header

```asm
.set MULTIBOOT_HEADER_MAGIC, 0x1BADB002
.set MULTIBOOT_HEADER_FLAGS, 0x00000003   /* page-align modules, memory info */

.section .multiboot, "a"
.align 4
    .long MULTIBOOT_HEADER_MAGIC
    .long MULTIBOOT_HEADER_FLAGS
    .long -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)
```

The Multiboot v1 header is three 32-bit words:

* `magic = 0x1BADB002` identifies the image.
* `flags = 0x3` asks the bootloader to align modules on page boundaries
  (bit 0) and to provide the memory map (`mem_lower`/`mem_upper`, bit 1).
  Paging is not requested; the kernel enables it later.
* The third word is the checksum: magic + flags + checksum must equal zero
  modulo 2^32. `-(magic + flags)` guarantees that.

The header must be 4-byte aligned and within the first 8192 bytes of the file.
Because the first LOAD segment starts at file offset `0x1000` and the header is
its first bytes, the header sits at offset `0x1000`, which satisfies both rules.
`grub-file --is-x86-multiboot build/kernel.elf` can verify this if the
`grub-file` utility is installed.

On entry the bootloader guarantees:

* 32-bit protected mode, flat address space, paging off, interrupts disabled;
* `%eax = 0x2BADB002` (`MULTIBOOT_BOOTLOADER_MAGIC`);
* `%ebx` = physical address of the `struct multiboot_info`;
* the CPU is otherwise in an undefined state, so the kernel must set up its own
  stack and segments.

`src/multiboot.h` defines the subset of the structure the kernel reads:

```c
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
```

The real Multiboot structure continues with drive, ROM and framebuffer fields.
The kernel only reads the fields shown, and `pmm_init` only reserves
`sizeof(struct multiboot_info)`, not the whole extended structure. This is safe
in practice because the structure is normally placed in low memory (which is
reserved wholesale) and because the kernel does not use modules or framebuffer
data. See the limitations in `11-end-to-end.md`.

The mmap entries look like this:

```c
struct multiboot_mmap_entry {
    uint32_t size;   /* bytes after this field, usually 20 */
    uint64_t addr;
    uint64_t len;
    uint32_t type;   /* 1 = available RAM */
} __attribute__((packed));
```

The linked list is byte-packed: the next entry starts at
`current + size + sizeof(size)`, not at a fixed stride. `pmm.c` iterates exactly
that way.

## 1.5 `_start`, instruction by instruction

```asm
_start:
    cli
    mov $stack_top, %esp

    mov %eax, %edx
    mov %ebx, %esi

    mov $__bss_start, %edi
    mov $__bss_end, %ecx
    sub %edi, %ecx
    xor %eax, %eax
    rep stosb

    push %esi
    push %edx
    call kernel_main

1:  cli
    hlt
    jmp 1b
```

### Step 1: disable interrupts

`cli` is redundant if the bootloader honours the specification, but it makes
the entry point safe if a firmware or chainloader left `IF` set. No interrupt
can arrive before the IDT exists.

### Step 2: install the boot stack

The stack lives in `.bss`:

```asm
.section .bss
.align 16
stack_bottom:
    .skip 16384
stack_top:
```

`%esp = stack_top` selects the first address *above* the 16 KiB region, which is
correct for a downward-growing x86 stack. `stack_top` is 16-byte aligned.
Note that the subsequent `push` of two arguments and the `call` leave
`kernel_main` with `%esp ≡ 4 (mod 16)`, whereas the i386 System V ABI expects
`%esp ≡ 12 (mod 16)` at function entry. The build tolerates this because the
kernel uses no aligned SSE accesses and `kernel_main`'s prologue only pushes
integer registers; a strict ABI-conforming entry would add 8 bytes of padding
before the arguments or realign the stack in the prologue. Nothing has been
written to `.bss` yet, so the stack contents are irrelevant.

Why a stack in `.bss` is safe: the zeroing loop below will also zero the 16 KiB
stack area. That is fine because the loop itself does not use the stack (no
calls, no pushes) and `%esp` points just past the area. This ordering is
deliberate and is called out in the source comment.

### Step 3: save the Multiboot arguments

`rep stosb` writes `%al` to `[%edi]`, increments `%edi`, and decrements `%ecx`.
It therefore destroys `%eax`, `%ecx` and `%edi`. The magic (`%eax`) and info
pointer (`%ebx`) must survive until the call, so they are copied to `%edx` and
`%esi`, which the zeroing loop does not touch.

### Step 4: zero `.bss`

* `%edi = __bss_start`
* `%ecx = __bss_end - __bss_start` (byte count)
* `%eax = 0`
* `rep stosb` fills the whole range with zero.

Multiboot does not guarantee that `.bss` is zeroed. C code assumes static
storage starts at zero (`page_directory`, the IDT, the bitmaps and the GDT
array all rely on this), so this loop is required. The linker places `COMMON`
inside `.bss`, so tentative definitions are covered too.

### Step 5: call `kernel_main(magic, info)`

SysV i386 cdecl passes arguments right-to-left on the stack:

```asm
push %esi      ; info  (second argument)
push %edx      ; magic (first argument)
call kernel_main
```

At entry to `kernel_main`, the stack looks like:

```
 higher addresses
 +------------------+
 | info             |  <- 8(%esp)
 +------------------+
 | magic            |  <- 4(%esp)
 +------------------+
 | return address   |  <- (%esp)
 +------------------+
 lower addresses
```

`kernel_main` is declared `void kernel_main(uint32_t magic, uint32_t info)`, so
the compiler reads exactly those two slots. The caller does not clean up the
arguments because it never returns.

### Step 6: halt forever

If `kernel_main` ever returns, `cli; hlt` inside an infinite loop stops the CPU
deterministically instead of executing whatever bytes follow. `hlt` wakes on an
interrupt, but interrupts are disabled, so it stays halted until reset.

## 1.6 How the bootloader finds and loads the image

For GRUB and for `qemu -kernel`, the flow is roughly:

1. Read the ELF headers and verify `elf32-i386` / `i386`.
2. Scan the first 8 KiB for the Multiboot magic and validate the checksum.
3. Allocate memory for each `PT_LOAD` segment and copy the file bytes.
4. Zero the part of a segment beyond `p_filesz` up to `p_memsz` (this includes
   `.bss`, although `_start` zeroes it again).
5. Build the `multiboot_info` structure and the memory map in available RAM.
6. Enter `_start` in protected mode with `%eax`/`%ebx` set.

The three LOAD segments of the current image are:

| Segment | Virt/Phys | File size | Mem size | Flags |
|---|---|---|---|---|
| 0 | `0x00100000` | `0x21EC` | `0x21EC` | R+X |
| 1 | `0x00103000` | `0x0D38` | `0x0D38` | R |
| 2 | `0x00104000` | `0` | `0x470A0` | R+W |

Segment 2 is `.bss`: zero file bytes, `0x470A0` bytes of memory. It ends at
`0x0014B0A0`, exactly `__bss_end`.

## 1.7 Things to notice / common pitfalls

* The boot stack is inside the kernel image, so the PMM reservation of
  `[1 MiB, kernel_end)` also reserves the stack. No separate guard page exists;
  the stack grows downward from `0x00108000`, so an overflow writes below
  `stack_bottom` (`0x00104000`) into the tail of `.rodata` — keymaps, the
  command table, the stub table and string literals. Because `.rodata` is
  mapped writable, the corruption is silent. Objects above `stack_top` such as
  `term_col`, the GDT and the IDT are not reached by normal downward growth.
* The kernel assumes the bootloader already loaded a flat GDT with a code
  segment usable at `0x00100000`. It does not reload segments until
  `gdt_init` runs. `vga_*` calls before `gdt_init` use those bootloader
  segments. This works with GRUB and QEMU, but it is an implicit dependency.
* `.bss` zeroing uses `rep stosb`, which requires `%es` to describe the same
  flat space as `%ds`. The bootloader guarantees flat segments; the kernel does
  not set `%es` until `gdt_flush`.
* There is no relocation or position independence. The image must be loaded at
  its link address (`0x00100000`). Multiboot loaders honour `p_paddr`.
* The header does not request a video mode (flag bit 2), so the kernel relies on
  the firmware leaving the machine in 80x25 text mode at `0xB8000`.

Next: [02-gdt.md](02-gdt.md).
