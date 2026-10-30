# bnOS source walkthrough

This directory explains the bnOS source code step by step, subsystem by
subsystem, in the order the kernel actually uses them.

bnOS is a freestanding 32-bit x86 kernel. It boots via Multiboot v1, installs a
flat GDT, an IDT with 48 gates, remaps the 8259A PIC, drives a PS/2 keyboard,
manages physical frames with bitmaps, enables identity paging, writes to the VGA
text buffer and runs a small shell. There is no libc, no scheduler, no user
mode and no virtual memory allocator.

## Reading order

| # | Document | What it explains |
|---|---|---|
| 0 | [README.md](README.md) | This overview, source map, build and run |
| 1 | [01-boot-and-linker.md](01-boot-and-linker.md) | `Makefile`, `linker.ld`, `boot.S`, Multiboot header, stack and `.bss` |
| 2 | [02-gdt.md](02-gdt.md) | `gdt.c`, `gdt_flush.S`, segment descriptors and selectors |
| 3 | [03-idt-and-interrupts.md](03-idt-and-interrupts.md) | `idt.c`, `isr.S`, interrupt frames, dispatch, exceptions, IRQs |
| 4 | [04-pic.md](04-pic.md) | `pic.c`, 8259A remap, masks and EOI |
| 5 | [05-keyboard.md](05-keyboard.md) | `keyboard.c`, scancode set 1, ring buffer, blocking reads |
| 6 | [06-vga.md](06-vga.md) | `vga.c`, text cells, cursor, scrolling, custom `printf` |
| 7 | [07-pmm.md](07-pmm.md) | `pmm.c`, Multiboot memory map, frame bitmaps, alloc/free |
| 8 | [08-paging.md](08-paging.md) | `paging.c`, 32-bit paging, identity map, map/unmap, TLB |
| 9 | [09-shell.md](09-shell.md) | `shell.c`, line editing, tokenizer, command table |
| 10 | [10-lib.md](10-lib.md) | `lib.c`, freestanding memory and string helpers |
| 11 | [11-end-to-end.md](11-end-to-end.md) | Full boot timeline, keypress/command/fault traces, invariants, limits |

If you only have ten minutes, read 1, 3, 7, 8 and 11.

## High-level picture

```
 firmware / QEMU / GRUB
          |
          | Multiboot v1: %eax = 0x2BADB002, %ebx = mbi
          v
   +----------------+      boot.S
   |    _start      |  set stack, zero .bss, call kernel_main
   +----------------+
          |
          v
   +----------------+      kernel.c
   |  kernel_main   |  banner, init order, enable IRQ1, run shell
   +----------------+
      |    |    |    |    |
      |    |    |    |    +--> shell_run() -- keyboard -- VGA
      |    |    |    +-------> paging_init()  -- needs PMM
      |    |    +------------> pmm_init()     -- needs Multiboot info
      |    +-----------------> keyboard_init(), pic_remap(), idt_init()
      +----------------------> gdt_init()
```

At run time the machine is in ring 0 protected mode with a flat 4 GiB address
space, interrupts enabled, and 4 KiB paging enabled with an identity map. Every
physical address the PMM hands out is directly usable as a pointer.

## Source map

| File | Role | Key entry points |
|---|---|---|
| `src/boot.S` | Multiboot header, boot stack, `.bss` zeroing | `_start` |
| `src/kernel.c` | Banner, init sequence, idle | `kernel_main` |
| `src/gdt.c` | Build three GDT descriptors | `gdt_init` |
| `src/gdt_flush.S` | `lgdt`, reload segments, far jump | `gdt_flush` |
| `src/idt.c` | 256-entry IDT, 48 gates, IRQ table, exception reporting | `idt_init`, `isr_dispatch` |
| `src/isr.S` | 48 stubs, common save/restore frame, `iret` | `isr0..isr47`, `isr_common` |
| `src/pic.c` | 8259A remap, mask/unmask, EOI | `pic_remap`, `pic_unmask`, `pic_send_eoi` |
| `src/keyboard.c` | IRQ1 handler, set-1 decode, ring buffer | `keyboard_init`, `keyboard_getchar` |
| `src/pmm.c` | Bitmap physical frame allocator | `pmm_init`, `pmm_alloc_page`, `pmm_free_page` |
| `src/paging.c` | Page directory/tables, identity map, map/unmap | `paging_init`, `paging_map_page` |
| `src/vga.c` | 80x25 text console and formatted output | `vga_putc`, `vga_printf` |
| `src/shell.c` | Line editing, tokenizer, commands | `shell_run` |
| `src/lib.c` | `memset`, `memcpy`, `memmove`, `str*`, `strtoul` | — |
| `src/io.h` | Port I/O and interrupt helpers | `inb`, `outb`, `sti`, `cli` |
| `src/multiboot.h` | Multiboot info and mmap structures | — |
| `linker.ld` | ELF32 at 1 MiB, `.multiboot` first | — |

### Where each file is explained

| Source file | Document |
|---|---|
| `Makefile`, `linker.ld`, `src/boot.S` | [01](01-boot-and-linker.md) |
| `src/gdt.c`, `src/gdt.h`, `src/gdt_flush.S` | [02](02-gdt.md) |
| `src/idt.c`, `src/idt.h`, `src/isr.S` | [03](03-idt-and-interrupts.md) |
| `src/pic.c`, `src/pic.h`, `src/io.h` | [04](04-pic.md) |
| `src/keyboard.c`, `src/keyboard.h` | [05](05-keyboard.md) |
| `src/vga.c`, `src/vga.h` | [06](06-vga.md) |
| `src/pmm.c`, `src/pmm.h`, `src/multiboot.h` | [07](07-pmm.md) |
| `src/paging.c`, `src/paging.h` | [08](08-paging.md) |
| `src/shell.c`, `src/shell.h` | [09](09-shell.md) |
| `src/lib.c`, `src/lib.h` | [10](10-lib.md) |
| `src/kernel.c` | [11](11-end-to-end.md) and every subsystem document |

## Build and run

```sh
make            # build/kernel.elf
make run        # qemu-system-i386 -kernel build/kernel.elf
make clean
```

Requirements: `gcc` with 32-bit support (`gcc -m32` must be able to link/compile
i386), `ld`, and `qemu-system-i386`. The image is a Multiboot ELF, so GRUB can
also load it with `multiboot /boot/kernel.elf`.

## Conventions used in these documents

* Addresses are physical unless explicitly called virtual.
* Code is quoted in the same style as the repository (4 spaces, `/* */`).
* Line numbers are not used as anchors because they move; function names and
  quoted code are.
* The "current build" figures come from `readelf`/`nm` on this machine with
  `gcc` and may shift slightly with a different compiler.
* "Frame" and "page" both mean a 4 KiB-aligned 4096-byte block. "Page" is used
  for the virtual/paging view, "frame" for the physical view.

## The kernel in one paragraph

`_start` receives Multiboot's magic and info pointer, switches to a 16 KiB
stack inside `.bss`, zeroes `.bss`, and calls `kernel_main`. The kernel prints a
banner, loads a flat GDT, builds an IDT with stubs for vectors 0–47, remaps the
PIC to 0x20–0x2F and masks every IRQ, decodes the keyboard on IRQ1, builds a
frame bitmap from the Multiboot memory map (reserving low 1 MiB, the kernel and
Multiboot data), builds an identity page map for every managed frame, unmask
IRQ1, enables interrupts and drops into a blocking shell. A keypress becomes an
IRQ, an IDT gate, an assembly stub, a C dispatcher, an ASCII byte in a ring
buffer and finally a shell command. `alloc`/`free` exercise the PMM and
`vmtest` exercises map/write/read/unmap at `0xC0000000`.

## Verification

The walkthrough was checked against the actual tree:

* `make` builds `build/kernel.elf` cleanly with the documented flags.
* ELF layout, section addresses and symbol addresses come from `readelf -h -l
  -S` and `nm -n` on that binary.
* The boot banner, `mem`, `paging`, `alloc` and `vmtest` output were captured
  from a live QEMU 10.1.5 boot by saving VGA memory at `0xB8000` through the
  monitor (`pmemsave`). That is where the concrete values 32,736 total frames,
  32,371 free, CR3 `0x14A000`, and the first free frame `0x16D000` come from.
* The MMAP and command-line placement (`mbi` at `0x9500`, cmdline at
  `0x14C000`) were read from guest memory and explain the one-frame difference
  from the simplified worked example in `07-pmm.md`.
