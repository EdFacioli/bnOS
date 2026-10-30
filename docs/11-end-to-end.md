# 11. End-to-end: boot, interrupts, commands and failure paths

This document ties the subsystems together. It follows the kernel from reset to
the shell prompt, then traces a keypress, a command, a virtual-memory test and
a page fault. It ends with invariants, limitations and debugging advice.

## 11.1 Boot timeline

| Step | Where | What happens |
|---|---|---|
| 1 | firmware / bootloader | POST, load the ELF at 1 MiB, build the Multiboot info and memory map, enter protected mode, jump to `_start` |
| 2 | `boot.S:_start` | `cli`, set `%esp` to `stack_top`, save magic/info in `%edx`/`%esi` |
| 3 | `boot.S:_start` | zero `[__bss_start, __bss_end)` with `rep stosb` |
| 4 | `boot.S:_start` | push `info`, push `magic`, `call kernel_main` |
| 5 | `kernel.c:kernel_main` | check `magic == 0x2BADB002`, derive `mbi` or NULL |
| 6 | `kernel.c` | `vga_clear()`, print banner, magic, info address |
| 7 | `gdt_init` | build null/code/data descriptors, `lgdt`, reload segments, far jump |
| 8 | `idt_init` | install 48 gates, `lidt`; vectors 48–255 stay not-present |
| 9 | `pic_remap` | master → `0x20`, slave → `0x28`, mask all 16 IRQs |
| 10 | `keyboard_init` | drain the 8042, register `keyboard_irq` for IRQ1 |
| 11 | `pmm_init` | size bitmaps from the mmap, free available ranges, reserve low 1 MiB, kernel, mbi, mmap, cmdline |
| 12 | `paging_init` | allocate page tables, build identity map for all managed frames, load CR3, set CR0.PG |
| 13 | `kernel.c` | print CPU/GDT/IDT/PIC/memory/paging summary |
| 14 | `kernel.c` | `pic_unmask(1)`, `interrupts_enable()` (`sti`) |
| 15 | `shell_run` | print hint, prompt, block in `keyboard_getchar` |

### Why this order?

* **VGA first.** Everything else reports through it. It only needs the
  bootloader's flat segments and `0xB8000`, both available immediately.
* **GDT before IDT.** Every IDT gate hard-codes selector `0x08`. Loading the
  IDT before the GDT would leave gates pointing at the bootloader's code
  segment, which may not cover the kernel.
* **IDT before PIC.** Once the PIC is remapped to `0x20`–`0x2F`, an unexpected
  IRQ must find a gate. More importantly, any CPU exception during the rest of
  boot must be reportable instead of triple-faulting.
* **Keyboard before PMM/paging.** Registering the handler is harmless while
  IRQ1 is masked. It could be done later, but doing it here keeps all
  interrupt-source setup together.
* **PMM before paging.** `paging_init` needs `pmm_total_pages()` for the map
  limit and `pmm_alloc_kernel_page()` for page tables. Enabling paging before
  the PMM would leave no allocator for tables.
* **Paging before `sti`.** The shell should run with the final address space.
  Enabling paging after interrupts would expose a window where an IRQ handler
  runs in physical mode (harmless here, but conceptually wrong).
* **`sti` last.** No maskable interrupt can be delivered until every handler,
  the PIC mask and the page tables are ready. A key pressed during boot stays
  latched in the 8042 and is delivered immediately after `sti`.

### What `kernel_main` prints

```
  bnOS - 32-bit x86 kernel
  ----------------------------------------------
  VGA text buffer : 0x000B8000 (80x25 char cells)
  Multiboot magic : 0x2BADB002 (valid)
  Multiboot info  : 0x0000....
  CPU mode        : protected mode, 32-bit flat
  GDT             : flat kernel code/data loaded
  IDT             : 32 exceptions + 16 IRQ gates
  PIC             : remapped to 0x20..0x2F, IRQ1 ready
  Memory          : 127 MiB total, 32371 pages free (126 MiB)
  Paging          : enabled, identity 127 MiB, CR3=0x14A000
  ----------------------------------------------
Type 'help' for available commands.
bnos>
```

The memory and paging figures depend on the firmware map, QEMU's `-m` setting
and how many page tables were allocated. The addresses depend on the compiler
and link.

## 11.2 Physical and virtual layout

```
Physical address space (identity mapped)

0x00000000  +---------------------------+
            | IVT, BDA, EBDA, BIOS ROM  |  reserved (low 1 MiB)
0x000A0000  | VGA / MMIO holes          |
0x000B8000  | VGA text buffer (4 KiB)   |
0x000C0000  | option ROMs               |
0x00100000  +---------------------------+
            | kernel .text              |
            | kernel .rodata            |
            | kernel .bss:              |
            |   boot stack (16 KiB)     |
            |   IDT, GDT, IRQ table     |
            |   alloc_bitmap (128 KiB)  |
            |   page_bitmap (128 KiB)   |
            |   page directory (4 KiB)  |
0x0014B0A0  +---------------------------+  __bss_end
            | free frames               |
            | ...                       |
            | page tables, vmtest frame |
            | ...                       |
top of RAM  +---------------------------+
            | reserved ACPI/firmware    |
0xFFFFFFFF  +---------------------------+
```

Virtual view after `paging_init`:

* `0x00000000`–`limit*4096` maps 1:1 to physical.
* `0xC0000000` is unmapped until `vmtest` adds a mapping.
* Addresses at or above `limit*4096` are unmapped (PDE not present).
* There is no higher-half kernel, no guard page and no user space.

## 11.3 A keypress, step by step

1. **Key press.** The keyboard sends a scancode (e.g. `0x1E` for `a`).
2. **8042.** The controller latches the byte and asserts IRQ1.
3. **8259A.** The master PIC (IRQ1 enabled, vector base `0x20`) raises `INT`
   with vector `0x21`.
4. **CPU.** Reads gate `0x21`, loads `CS=0x08`, clears `IF`, pushes
   `eflags`, `cs`, `eip`.
5. **Stub `isr33`.** `pushl $0` (dummy error), `pushl $33` (vector), jump to
   `isr_common`.
6. **`isr_common`.** `pusha` saves eight registers; `ds/es/fs/gs` are set to
   `0x10`.
7. **C dispatch.** `pushl %esp; call isr_dispatch`.
8. **`isr_dispatch`.** Vector 33 ≥ 32, so `irq = 1`.
9. **`keyboard_irq`.** `inb(0x60)` reads and acknowledges the scancode. It
   checks `0xE0`, Shift (`0x2A`/`0x36`), Caps (`0x3A`), break bit, then calls
   `translate` and `buffer_push`.
10. **EOI.** `pic_send_eoi(1)` writes `0x20` to port `0x20`.
11. **Return.** `popa`, discard vector/error, `iret` restores `eip/cs/eflags`
    and re-enables interrupts.
12. **Shell.** The CPU was in `sti; hlt` inside `keyboard_getchar`; the IRQ
    wakes it. The loop rechecks the buffer, disables interrupts, reads the
    byte, advances `tail`, re-enables, returns the character.
13. **Echo.** `read_line` stores the byte and calls `vga_putc`, which writes
    the cell and moves the hardware cursor.
14. **Enter.** `'\n'` terminates the line; `execute` tokenizes and dispatches.

Interrupt context is short: one port read, a few branches, one byte push. All
other work (echo, tokenize, command execution) runs in the main shell context
with interrupts enabled.

## 11.4 A command, step by step

Take `echo hello world`:

1. `read_line` accumulates `e`, `c`, `h`, `o`, ...; each character is echoed.
2. Enter writes the NUL and returns.
3. `execute` calls `tokenize(line_buffer, argv, 8)`.
4. `tokenize` writes NULs over the two spaces:
   `"echo\0hello\0world\0"`, `argc = 3`.
5. The lookup loop compares `argv[0]` with each command name; `strcmp` finds
   `"echo"`.
6. `cmd_echo(3, argv)` prints `hello`, a space, `world`, and a newline.
7. `shell_run` loops, prints the prompt, blocks again.

`help`, `clear`, `about` and `halt` follow the same path with different
functions. `mem`, `paging`, `alloc`, `free` and `vmtest` additionally call into
the PMM and paging modules.

## 11.5 `vmtest` path

`vmtest` is the only command that creates a non-identity mapping:

1. `paging_is_mapped(0xC0000000)`: PDE 768 is normally not present (unless RAM
   ≥ 3 GiB), so it returns 0.
2. `pmm_alloc_page()` returns a free physical frame, e.g. `0x0016D000`.
3. `paging_map_page(0xC0000000, 0x0016D000, 1)`:
   * `page_table_for` sees PDE 768 absent, allocates a kernel page for the
     table (e.g. `0x0016E000`), zeroes it, installs the PDE;
   * PTE 0 is set to `0x0016D000 | P | RW`;
   * `mapped_pages` increments;
   * `invlpg(0xC0000000)` flushes any stale entry.
4. The shell writes `0xDEADBEEF` through `0xC0000000`. The CPU walks PDE 768 →
   PTE 0 → physical `0x0016D000`, and stores the value.
5. The shell reads it back through the same mapping. `volatile` forces a real
   load.
6. `paging_unmap_page(0xC0000000)` zeroes PTE 0, decrements `mapped_pages`,
   `invlpg`.
7. `pmm_free_page(0x0016D000)` clears the page and alloc bits and increments
   `free_pages`.
8. The command prints the result.

Afterwards, PDE 768 still points to the page table, but PTE 0 is zero. A second
`vmtest` reuses the table; it does not leak another page table. The physical
frame is back in the PMM and may be reused.

If `0xC0000000` is already identity mapped (≥3 GiB RAM), step 1 returns true
and the command prints "already mapped, skipped". The test cannot distinguish
the identity mapping from a real remap in that configuration.

## 11.6 A page fault, step by step

Suppose something reads `0xC0000000` before `vmtest` has mapped it:

1. The CPU walks CR3 → PDE 768. The PDE is zero (not present).
2. The CPU raises vector 14, pushes an error code (`0x0`: not present, read,
   kernel) and sets `CR2 = 0xC0000000`.
3. The IDT gate for 14 points to `isr14`. `ISR_ERR` does not push a dummy
   error code; it pushes vector 14.
4. `isr_common` saves registers, sets segments, calls `isr_dispatch`.
5. `isr_dispatch` sees `vector < 32`, prints:
   ```
   EXCEPTION 14: page fault (error=0x0) eip=0x...
     fault address : 0xC0000000
     cause         : page not present, read, kernel mode
   System halted.
   ```
6. The handler enters `for (;;) cli; hlt`. The machine stops.

If CR0.WP were set, a ring-0 write to a read-only mapping would set error bit
0 (`P=1`) and bit 1 (`W/R=1`); with WP=0 the kernel can write it without
faulting. A user-mode access would set bit 2, but there is no user mode. An
instruction fetch fault sets bit 4 on CPUs that report it.

A general protection fault (vector 13) is similar: e.g. `int $48` with a
not-present gate, or loading selector 0 into a segment register. It prints the
vector, name, error code and `eip`, then halts.

A double fault (vector 8) has no separate stack. If the current stack is
unusable (stack overflow into unmapped memory), the CPU may triple-fault and
reset instead of printing anything. The identity map covers the boot stack and
all of `.bss`, so a moderate overflow corrupts data rather than faulting; a
wild `%esp` can still triple-fault.

## 11.7 State after boot

| Subsystem | State |
|---|---|
| CPU | ring 0, protected mode, paging on, interrupts on |
| GDT | 3 entries: null, flat code `0x08`, flat data `0x10` |
| IDT | 48 gates, vectors 0–47; 48–255 not present |
| PIC | master base `0x20`, slave base `0x28`, only IRQ1 unmasked |
| Keyboard | set-1 decoder, Shift/Caps tracked, 256-byte ring buffer |
| PMM | two 128 KiB bitmaps, low 1 MiB + kernel + Multiboot reserved |
| Paging | 32-bit, 4 KiB, identity map of all managed frames, CR3 loaded |
| VGA | 80×25 text, light grey on black, cursor tracked |
| Shell | blocking loop, 10 commands, 127-char lines, 8 args |
| Timers | none (IRQ0 masked) |
| Processes | none |
| Filesystem | none |
| Network | none |

## 11.8 Failure and degradation paths

### Invalid Multiboot magic

`kernel_main` sets `valid_mbi = 0` and passes `mbi = NULL` to `pmm_init`.
`detect_total_pages(NULL)` returns 0 → `pmm_init` substitutes `MAX_PAGES`.
`free_available_ranges(NULL)` does nothing, so **every** frame stays marked
used. `free_pages = 0`.

`paging_init` then computes `limit = 1,048,576`, `tables = 1024`, calls
`pmm_alloc_kernel_page()` and gets 0 on the first table. It prints
`paging: out of memory building page tables` and returns with paging still
disabled.

The banner reports the unexpected magic, `Memory: 4096 MiB total, 0 pages free
(0 MiB)` and `Paging: disabled`. `pic_unmask(1)` and `sti` still run, and the
shell works in flat physical mode. `mem` shows zero free; `alloc` and `vmtest`
both report "out of physical memory"; `paging` reports disabled. This is a
graceful but misleading degradation: the kernel pretends 4 GiB exists but
cannot use any of it. A stricter design would halt with a diagnostic instead.

### Page-table allocation failure

If the PMM runs out while building the identity map, `paging_init` prints an
error and returns without enabling paging. The partially built page directory
is ignored. The shell runs in physical mode. This can happen with a very small
`-m` value or a malformed map.

### No keyboard / no IRQ1

`shell_run` prints the prompt and blocks in `sti; hlt`. With no IRQ1 and no
timer, nothing wakes the CPU. The machine appears frozen but is actually
halted. There is no timeout, watchdog or polling fallback.

### Keyboard buffer overflow

If more than 256 characters arrive before the shell reads them (e.g. holding a
key during a long command), `buffer_push` drops the newest characters. The
oldest 256 are processed. There is no indication of loss.

### Stack overflow

The boot stack is 16 KiB at `0x00104000`–`0x00108000`, at the start of `.bss`;
below it is the tail of `.rodata`. The stack grows downward, so a deep call
chain or a large local array writes past `stack_bottom` into `.rodata`
(keymaps, the command table, the stub table and string literals). Because all
pages are mapped writable, that corruption is silent. It does not normally
reach `term_col`, the GDT, the IDT or the bitmaps, which are above `stack_top`.
There is no guard page and no stack canary. The shell's call depth is small, so
normal use is safe.

### Null pointer

Page 0 is identity mapped and writable. `*(int *)0 = 1` writes low physical
memory instead of faulting. Bugs that would be caught by a guard page are
silent here.

### Triple fault

A fault while delivering a fault (e.g. an invalid IDT entry, a bad stack, or a
segment error during `isr_common`) causes a double fault; a fault during the
double-fault handler causes a triple fault and a CPU reset. On QEMU the machine
reboots (unless `-no-reboot`). There is no diagnostic because no serial or
persistent log exists.

## 11.9 Invariants worth remembering

1. `_start` runs with the bootloader's flat segments; `gdt_init` replaces them.
2. `_start` sets `%esp` to a 16-byte-aligned `stack_top`; the two argument
   pushes and the `call` leave `kernel_main` at `%esp ≡ 4 (mod 16)`, which is
   not the ABI's expected `12 (mod 16)` but is harmless for this non-SSE build.
3. `.bss` is zero before `kernel_main`; all static state starts at zero.
4. Frame 0 and all of low 1 MiB are reserved.
5. `[1 MiB, __bss_end)` is reserved, including the PMM bitmaps themselves.
6. `alloc_bitmap` is set only for frames returned by `pmm_alloc_page`.
7. `free_pages` equals the number of zero bits in `[0, total_pages)`.
8. All PMM-returned addresses are 4 KiB aligned and identity mapped after
   `paging_init`.
9. Every identity PTE is `P | RW`; there are no user, global or NX mappings.
10. Page tables are allocated with `pmm_alloc_kernel_page` and are permanently
    reserved.
11. A PTE change is followed by `invlpg` for that virtual address.
12. `isr_common`'s stack layout matches `struct isr_frame`.
13. Interrupt gates clear `IF`; handlers do not re-enable interrupts.
14. Every IRQ path sends EOI, even with no registered handler.
15. The keyboard ring buffer has one producer (IRQ1) and one consumer (shell).
16. `line_buffer` is always NUL-terminated before `execute`.
17. `argc` never exceeds `ARGS_MAX` (8).
18. `last_alloc` is either 0 or a frame returned by `alloc`.
19. The VGA cursor always matches `term_row`/`term_col` after `vga_putc`.
20. `shell_run` never returns; `kernel_main`'s `halt` is unreachable.

## 11.10 Debugging

### QEMU monitor

Start QEMU with the monitor on stdio or a socket:

```sh
qemu-system-i386 -kernel build/kernel.elf -monitor stdio
```

Useful commands:

```
info registers        # EAX..EDI, EIP, EFLAGS, CR0, CR2, CR3, ...
info mem              # virtual memory mappings
info tlb              # TLB entries
x/16xb 0xB8000        # inspect VGA text memory
x/10i $eip            # disassemble at the current instruction
screendump screen.ppm # capture the display
```

### GDB

```sh
qemu-system-i386 -kernel build/kernel.elf -s -S
gdb build/kernel.elf
(gdb) target remote :1234
(gdb) layout asm
(gdb) break kernel_main
(gdb) continue
(gdb) info registers cr0 cr2 cr3
(gdb) x/32x 0x0014A000   # page directory
```

`-s` opens a GDB stub on port 1234; `-S` freezes the CPU at reset until GDB
continues. This is the most reliable way to inspect the IDT, page tables and
stack frame during an interrupt.

### QEMU logging

```sh
qemu-system-i386 -kernel build/kernel.elf -no-reboot -no-shutdown \
    -d int,cpu_reset,guest_errors -D qemu.log
```

`-d int` logs exceptions and interrupts with vectors and error codes; `cpu_reset`
logs triple faults; `guest_errors` logs invalid device accesses. `-no-reboot`
keeps a triple fault from restarting silently.

### Static inspection

```sh
readelf -h -l -S build/kernel.elf   # ELF type, segments, sections
nm -n build/kernel.elf              # symbols sorted by address
objdump -d -Mintel build/kernel.elf # disassembly
grub-file --is-x86-multiboot build/kernel.elf
```

`nm -n` is how the actual addresses in these documents were obtained.

### Debug console

QEMU's `-debugcon` port `0xE9` is a convenient early-output channel:

```c
static inline void debug_putc(char c) { outb(0xE9, (uint8_t)c); }
```

Run with `qemu-system-i386 -debugcon stdio -kernel build/kernel.elf` and writes
appear on the host terminal. The kernel does not currently use it, but it is a
good first addition for debugging before VGA or paging is up.

## 11.11 Extension guide

### Add a shell command

1. Declare `static void cmd_foo(int argc, char **argv);` near the other
   prototypes.
2. Add `{ "foo", "description", cmd_foo }` to `commands[]`.
3. Implement `cmd_foo`; cast unused parameters to `void`.
4. No other file changes are needed: `help`, lookup and dispatch are driven by
   the table.

### Add an IRQ handler

1. Implement `static void foo_irq(void)`.
2. Call `irq_register_handler(irq, foo_irq)` during init.
3. Initialize the device and clear any pending interrupt in the handler.
4. Unmask with `pic_unmask(irq)` after all setup.
5. The handler runs with interrupts disabled; keep it short and defer work to
   the main loop.
6. For a slave IRQ (≥8), `pic_unmask` also enables the cascade automatically.

### Add a timer

1. Program the PIT (channel 0, ports `0x40`/`0x43`) for a tick rate.
2. Register a handler for IRQ0.
3. `pic_unmask(0)`.
4. Increment a `volatile uint64_t ticks` in the handler.
5. A sleep loop can then `sti; hlt` until `ticks` advances.
6. A scheduler would need context switching, a TSS for ring transitions, and
   per-task stacks — a much larger change.

### Add a virtual allocator / heap

1. Reserve a virtual range that is **not** identity mapped (above the
   `paging_init` limit, and avoiding `vmtest`'s `0xC0000000`).
2. Keep a bitmap or free list of virtual pages.
3. On allocation, `pmm_alloc_page()` a frame and `paging_map_page()` it at the
   chosen virtual address.
4. On free, `paging_unmap_page()` and `pmm_free_page()`.
5. Add page-table reclamation if the range is large.
6. Remember that the kernel currently uses physical pointers everywhere;
   switching to a heap does not require changing that, but a higher-half
   kernel would.

### Add user mode

This is a major change:

1. Add user code/data descriptors at indices 3 and 4 (selectors `0x1B` and
   `0x23`, RPL 3) and a TSS descriptor.
2. Load the TSS with `ltr` (a TSS selector such as `0x28`).
3. Map user pages with U/S=1 and set CR0.WP.
4. Build an `iret` frame (SS, ESP, EFLAGS, CS, EIP) to enter ring 3.
5. Add a syscall gate (DPL 3) and a handler that validates user pointers.
6. Add per-process page directories and a scheduler.
7. The current IDT gate DPL is 0, so `int` from ring 3 would #GP; syscall gates
   must be DPL 3.

### Add serial output

1. Initialize COM1 at `0x3F8` (baud divisor, 8N1, FIFO).
2. Write bytes to `0x3F8` when the line-status register bit 5 is set.
3. Optionally enable IRQ4 and register a handler for input.
4. Serial is invaluable for logging before VGA/paging works.
5. QEMU: `-serial stdio`.

## 11.12 Known limitations (summary)

* No libc, no heap, no C++ runtime.
* No user mode, no processes, no scheduler, no syscalls.
* No timer, no sleep, no uptime.
* No filesystem, no disk driver, no network.
* No serial driver (VGA only).
* No PIC spurious-IRQ handling, no APIC/IOAPIC.
* No keyboard layout selection, LEDs, Ctrl/Alt, arrows or mouse.
* No terminal scrolling history; `clear` is the only screen management.
* No 64-bit, PAE, NX, 4 MiB pages or higher-half mapping.
* No demand paging, swapping, COW or virtual allocator.
* Identity map is all supervisor and writable, including `.text`/`.rodata`.
* Page 0 is mapped, so null dereferences do not fault.
* No stack guard, no canaries, no unwinder.
* No locking: PMM and VGA assume a single context.
* PMM is first-fit O(n) with no coalescing.
* Only the first 52 bytes of the Multiboot info, the mmap and the cmdline are
  reserved; modules and extended tables are not.
* Invalid Multiboot magic degrades to "4 GiB total, 0 free, paging disabled"
  instead of halting.
* `vmtest` is skipped on machines with ≥3 GiB of RAM.
* `vmtest` leaks one page table (PDE 768) permanently.
* `free` accepts `strtoul` octal/negative quirks.
* Keyboard buffer and line length are small; overflow is silent.

## 11.13 Glossary

| Term | Meaning |
|---|---|
| **frame** | a 4 KiB-aligned physical page |
| **page** | a 4 KiB-aligned virtual unit, or a frame in context |
| **PDE / PTE** | page-directory entry / page-table entry |
| **CR0.PG** | control-register bit that enables paging |
| **CR2** | control register holding the faulting linear address |
| **CR3** | control register holding the page-directory physical address |
| **GDT / IDT** | global descriptor table / interrupt descriptor table |
| **selector** | 16-bit segment reference (index, TI, RPL) |
| **gate** | an IDT entry describing an interrupt/trap handler |
| **IRQ** | hardware interrupt line into the PIC |
| **vector** | interrupt number 0–255 |
| **EOI** | end-of-interrupt command to the PIC |
| **PIC** | 8259A programmable interrupt controller |
| **Multiboot** | boot protocol used by GRUB and `qemu -kernel` |
| **mbi** | Multiboot info structure |
| **identity map** | virtual address equals physical address |
| **TLB** | translation lookaside buffer |
| **invlpg** | invalidate one TLB entry for a virtual page |
| **first fit** | allocate the lowest-numbered free frame |
| **freestanding** | no hosted C library or runtime |

## 11.14 Suggested exercises

1. Add an `uptime` command driven by a PIT IRQ0 tick counter.
2. Add a serial console and mirror `vga_write` to COM1.
3. Make `.text` and `.rodata` read-only by clearing `PTE_WRITE` for their
   ranges after `paging_init`, and set `CR0.WP`.
4. Add a heap on top of `paging_map_page`.
5. Add a guard page below the boot stack and move the stack out of `.bss`.
6. Add Ctrl-C handling (a control byte in the ring buffer) and cancel
   `read_line`.
7. Add command history with the arrow keys (requires decoding `0xE0 0x48` /
   `0xE0 0x50`).
8. Replace the two bitmaps with a buddy allocator and compare fragmentation.
9. Map the kernel at a higher-half address and keep a small identity map for
   boot.
10. Add a second `vmtest` that maps a frame at two different virtual addresses
    and verifies aliasing.
11. Add a page-table walker command that dumps PDE/PTE bits for an address.
12. Validate the Multiboot magic and memory map more strictly, and halt with a
    clear message on bad input.

Back to the [index](README.md).
