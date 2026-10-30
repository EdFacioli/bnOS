# 3. IDT, interrupt stubs and dispatch

Files: `src/idt.c`, `src/idt.h`, `src/isr.S`.

The IDT maps 256 possible interrupt vectors to handler routines. bnOS installs
gates for vectors 0–31 (CPU exceptions) and 32–47 (the 16 IRQs after PIC
remapping). Vectors 48–255 stay not-present, so an unexpected interrupt raises
a general protection fault instead of silently doing nothing.

## 3.1 The IDT entry format

Each gate is eight bytes:

```
 63                48 47      40 39       32
 +------------------+----------+-----------+
 |  offset 31:16    | type/attr|  selector |
 +------------------+----------+-----------+
 31                                16 15                 0
 +-----------------------------------+-------------------+
 |            offset 15:0            |       zero        |
 +-----------------------------------+-------------------+
```

`idt.c` mirrors it:

```c
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t type_attr;
    uint16_t offset_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));
```

`type_attr = 0x8E` decodes as:

| Bits | Field | Value | Meaning |
|---|---|---|---|
| 7 | P | 1 | present |
| 6–5 | DPL | 00 | ring 0 only |
| 4 | S | 0 | system descriptor |
| 3–0 | type | 0xE | 32-bit interrupt gate |

An **interrupt gate** clears `IF` on entry, so the CPU does not take another
maskable interrupt until the handler executes `iret`. A trap gate (`0xF`) would
leave `IF` unchanged. Using an interrupt gate keeps the keyboard handler and
the shared static buffers free of nested-interrupt races.

`selector = 0x08` is the kernel code segment from the GDT. The CPU loads `CS`
with it when the gate fires.

## 3.2 Installing the gates

```c
#define IDT_ENTRIES 256
#define KERNEL_CODE_SELECTOR 0x08
#define IDT_GATE_INTERRUPT 0x8E

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idt_descriptor;
static irq_handler_t irq_handlers[16];

extern const uint32_t isr_stub_table[48];
```

`idt` is a 2 KiB array in `.bss`. All entries start zeroed, which means
"not present". `isr_stub_table` is defined in `isr.S` as 48 32-bit addresses in
`.rodata`.

```c
static void idt_set_gate(uint8_t vector, uint32_t handler)
{
    idt[vector].offset_low = (uint16_t)(handler & 0xFFFF);
    idt[vector].offset_high = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[vector].selector = KERNEL_CODE_SELECTOR;
    idt[vector].zero = 0;
    idt[vector].type_attr = IDT_GATE_INTERRUPT;
}

void idt_init(void)
{
    for (uint32_t i = 0; i < 48; i++)
        idt_set_gate((uint8_t)i, isr_stub_table[i]);

    idt_descriptor.limit = (uint16_t)(sizeof(idt) - 1);
    idt_descriptor.base = (uint32_t)(uintptr_t)&idt;
    __asm__ volatile("lidt (%0)" : : "r"(&idt_descriptor));
}
```

Step by step:

1. For each of the 48 vectors, split the handler address into low and high
   halves and store them in the gate.
2. `selector` is always the kernel code segment.
3. `zero` must be zero.
4. `type_attr` is the same for all gates: present, ring 0, interrupt gate.
5. The IDTR limit is `sizeof(idt) - 1 = 2047`, covering all 256 entries.
6. `lidt` loads the pseudo-descriptor. Like `lgdt`, it does not touch `CS`; the
   next interrupt or exception uses the new table immediately.
7. Vectors 48–255 remain zero. A software `int $48` or a stray hardware vector
   raises #GP (vector 13), which is installed and reports the fault.

`idt_init` runs after `gdt_init` because the gates refer to selector `0x08`.
It runs before `pic_remap` and before interrupts are enabled.

## 3.3 The stubs: making every vector look the same

CPU exceptions are inconsistent: some push an error code, some do not. IRQs
never push one. C code wants one uniform frame, so each vector gets a small
assembly stub.

```asm
.macro ISR_NOERR num
.global isr\num
.type isr\num, @function
isr\num:
    pushl $0
    pushl $\num
    jmp isr_common
.endm

.macro ISR_ERR num
.global isr\num
.type isr\num, @function
isr\num:
    pushl $\num
    jmp isr_common
.endm
```

For a no-error vector, the stub pushes a dummy `0` and then the vector number.
For an error-code vector, the CPU has already pushed the error code, so the
stub only pushes the vector number. Either way the stack ends up with:

```
[vector] [error] [eip] [cs] [eflags]
```

from low to high addresses (the stack grows down, so `vector` is at the lowest
address).

The error-code vectors are exactly those the CPU defines this way:

| Vector | Exception | Error code meaning |
|---|---|---|
| 8 | double fault | always 0 |
| 10 | invalid TSS | selector / descriptor index |
| 11 | segment not present | selector |
| 12 | stack-segment fault | selector |
| 13 | general protection | selector or 0 |
| 14 | page fault | page-fault error bits |
| 17 | alignment check | always 0 |
| 21 | control protection | CET status |
| 29 | VMM communication | #VC error code |
| 30 | security exception | #SX error code |

Vectors 21, 29 and 30 only exist on newer CPUs; on a plain i386 they are
reserved. Marking them with `ISR_ERR` is harmless because they cannot fire
there.

The IRQ stubs (32–47) are all `ISR_NOERR`: the PIC does not push an error code.

## 3.4 The common path

```asm
isr_common:
    pusha
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    pushl %esp
    call isr_dispatch
    addl $4, %esp

    popa
    addl $8, %esp    /* discard vector and error code */
    iret
```

### `pusha`

`pusha` pushes, in order: `eax`, `ecx`, `edx`, `ebx`, the original `esp`,
`ebp`, `esi`, `edi`. Because the stack grows down, `edi` ends up at the lowest
address. The resulting layout from `%esp` upward is:

```
 offset   field
  0       edi
  4       esi
  8       ebp
 12       esp_dummy     (value of esp before pusha)
 16       ebx
 20       edx
 24       ecx
 28       eax
 32       vector
 36       error
 40       eip
 44       cs
 48       eflags
```

`idt.c` mirrors this exactly:

```c
/* Must match the stack layout built by isr_common in isr.S. */
struct isr_frame {
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t vector, error;
    uint32_t eip, cs, eflags;
};
```

There is no `ss`/`esp` in the frame because the CPU pushes those only on a
privilege-level change. All interrupts here occur in ring 0, so the CPU pushes
just `eflags`, `cs`, `eip` (plus an error code where applicable).

### Segment reload

`mov $0x10` into `ds/es/fs/gs` guarantees the C handler sees flat kernel data
segments even if an interrupt arrived while user code (not present yet) or a
different selector was loaded. `ss` is not reloaded here; the CPU already uses
the ring-0 stack, and the flat data segment is already active.

### Calling C

`pushl %esp` passes a pointer to the frame as the first argument to
`isr_dispatch`. The i386 System V ABI expects the stack to be 16-byte aligned
at a call site, but an interrupt can arrive at any instruction, so the entry
`%esp` is not guaranteed to have any particular alignment. `isr_common` does
not realign it. This works in the current build because `isr_dispatch` only
reads the frame through the pointer and the kernel is compiled without aligned
SSE accesses; a stricter implementation would save the old stack pointer,
align `%esp`, call C, and restore it before `popa`. If a future handler used
aligned SSE or a compiler option that assumes alignment, this would need to be
fixed.

`addl $4, %esp` discards the frame pointer after the call.

### Returning

`popa` restores the general-purpose registers (including the saved `esp`, which
`popa` ignores for the stack pointer itself). `addl $8, %esp` removes the
vector and error code that the stub/CPU pushed. `iret` pops `eip`, `cs` and
`eflags`; because the gate cleared `IF`, the saved `eflags` contains the `IF`
value from before the interrupt, so `iret` re-enables interrupts when
appropriate.

## 3.5 Dispatch

```c
void isr_dispatch(struct isr_frame *frame)
{
    if (frame->vector < 32) {
        ... print and halt ...
    }

    uint8_t irq = (uint8_t)(frame->vector - 32);
    if (irq < 16 && irq_handlers[irq] != 0)
        irq_handlers[irq]();
    pic_send_eoi(irq);
}
```

There are two paths.

### Exception path (vectors 0–31)

The handler prints the vector, the name from `exception_names`, the error code
and the faulting `eip`, then halts the CPU with `cli; hlt` forever. There is no
recovery, no signal, no process to kill and no panic stack trace.

For vector 14 (page fault) it additionally reads `CR2` (the faulting linear
address) and decodes the error bits:

| Bit | Mask | Meaning in the report |
|---|---|---|
| 0 | `0x1` | 0 = page not present, 1 = protection violation |
| 1 | `0x2` | 0 = read, 1 = write |
| 2 | `0x4` | 0 = kernel mode, 1 = user mode |
| 3 | `0x8` | reserved bit set in a paging structure |
| 4 | `0x10` | instruction fetch (only on CPUs that report it) |

The report is printed in light red and ends with `System halted.`.

### IRQ path (vectors 32–47)

`irq = vector - 32`. The handler table has 16 slots. If a handler is registered,
it is called with no arguments. Then `pic_send_eoi(irq)` acknowledges the
interrupt.

Important details:

* The EOI is sent **after** the handler, not before. This is correct for a
  normal (non-nested) handler: the PIC keeps the IRQ in service until the work
  is done, and no new IRQ of the same or lower priority is delivered while the
  handler runs (interrupts are disabled by the gate anyway).
* An IRQ with no registered handler is still acknowledged, so it does not
  wedge the PIC.
* `pic_send_eoi` is called even for spurious IRQ7/IRQ15. On real 8259A
  hardware, a spurious IRQ7 should not receive an EOI; doing so can drop a
  real IRQ that was being asserted. QEMU is forgiving. This is a known
  simplification.
* `isr_dispatch` is not reentrant: the gate clears `IF`, and the kernel never
  re-enables interrupts inside a handler.
* The handler runs on the current kernel stack, not a separate interrupt
  stack. There is no IST/TSS.

## 3.6 The stub table

```asm
.section .rodata
.align 4
.global isr_stub_table
isr_stub_table:
    .long isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7
    ...
    .long isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47
```

The table is an array of 32-bit function addresses in read-only memory.
`idt.c` declares it as `extern const uint32_t isr_stub_table[48]` and reads each
element. The linker resolves `isr0`…`isr47` to the stub addresses. Because the
table is in `.rodata`, it is mapped read-only by the page tables (the PTE has
only `P` and `RW`; `RW=1` actually makes it writable — see the note below).

Actually, in this kernel every identity-mapped PTE is created with
`PTE_PRESENT | PTE_WRITE`, so `.rodata` is writable despite the ELF flags. The
separation is a convention, not a hardware protection. Making it read-only
would require walking the page tables for the `.rodata` range and clearing
`PTE_WRITE` for those pages.

## 3.7 Interrupt flow diagram

```
   PS/2 key
      |
      v
  8042 controller
      |  IRQ1
      v
  8259A PIC  (vector 0x21 after remap)
      |
      v
  IDT[0x21]  ->  isr33
                   |  push $0, push $33
                   v
                isr_common
                   |  pusha, load ds/es/fs/gs = 0x10
                   |  push esp, call isr_dispatch
                   v
                isr_dispatch
                   |  vector 33 -> irq 1
                   |  irq_handlers[1]() = keyboard_irq
                   |  pic_send_eoi(1)
                   v
                isr_common
                   |  popa, add $8, iret
                   v
                interrupted code continues
```

## 3.8 Why not a trap gate, why not software interrupts?

* A trap gate leaves `IF` set, allowing nested interrupts. The shared
  `irq_handlers` table and the keyboard ring buffer are not protected by locks,
  so nested IRQs would create races. An interrupt gate is simpler and correct
  here.
* Software interrupts (`int $0x80`-style syscalls) are not used. There is no
  syscall interface. The same gates would work, but the stub convention still
  expects the vector/error pair on the stack, and user mode does not exist.
* The IDT has 256 slots for architectural completeness, but only 48 are
  populated. This keeps the table small (2 KiB) and makes stray vectors loud.

## 3.9 Edge cases and invariants

* `frame->vector < 32` must never return. The `for (;;) cli; hlt` loop is
  intentional.
* `frame->vector` is trustworthy only because the stub pushes it; the CPU does
  not. A bug in the stub would be reported as a wrong exception.
* Vector 8 (double fault) can itself fault if the stack is unusable. There is
  no separate double-fault stack (no TSS/IST), so a stack overflow may triple
  fault and reset the machine. This is acceptable for a minimal kernel.
* The direction flag is assumed clear. The System V i386 ABI requires `DF=0`
  on function entry, the compiler emits `cld` where needed, and the kernel
  never executes `std`. `isr_common` does not explicitly `cld`.
* `isr_common` does not save/restore floating-point or SSE state. The kernel
  does not use them, and GCC is invoked without `-msse` for the freestanding
  build. A future context switch would need `fxsave`/`fxrstor`.
* `isr_common` overwrites `ds/es/fs/gs` with selector `0x10` but does not save
  or restore the previous values. This is safe because the kernel always runs
  with those flat kernel data segments; a user-mode port would need to save
  them (or reload them from the frame) before returning.
* Interrupts are disabled inside every handler, so handlers must be short.
  The keyboard handler only reads one port and pushes a byte.

Next: [04-pic.md](04-pic.md).
