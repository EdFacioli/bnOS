# 4. 8259A PIC

Files: `src/pic.c`, `src/pic.h`, `src/io.h`.

The 8259A programmable interrupt controller sits between hardware devices and
the CPU's `INTR` pin. A PC/AT has two of them: a master at ports `0x20/0x21`
and a slave at `0xA0/0xA1`, with the slave cascaded into the master's IRQ2.
Each controller has eight inputs (IRQ0–IRQ7 and IRQ8–IRQ15).

bnOS only needs one input: IRQ1, the PS/2 keyboard. The driver remaps both
controllers away from the BIOS vector range, masks everything, and provides
unmask and EOI helpers.

## 4.1 Why remap?

After reset the 8259A delivers IRQ0–IRQ7 as vectors `0x08`–`0x0F` and
IRQ8–IRQ15 as `0x70`–`0x77`. Those collide with CPU exception vectors:
`0x08` is double fault, `0x09` is coprocessor segment overrun, `0x0D` is
general protection fault, and so on. If an IRQ arrived before the IDT was
configured, or if the IDT mapped vector 8 to a double-fault stub, the kernel
would misinterpret a timer tick as an exception.

The standard fix is to remap the master to `0x20`–`0x27` and the slave to
`0x28`–`0x2F`, placing all 16 IRQs after the 32 architectural exceptions.
That matches the 48 IDT gates installed by `idt_init`.

## 4.2 Ports and registers

| Port | Direction | Purpose |
|---|---|---|
| `0x20` | write | master command (ICW1, OCW2, OCW3) |
| `0x21` | read/write | master data (ICW2–ICW4) / interrupt mask (OCW1) |
| `0xA0` | write | slave command |
| `0xA1` | read/write | slave data / mask |

The same data port has different meanings depending on the initialization
phase and on which command word was written last. That is why the sequence
below must be exact.

`io_wait()` writes a zero byte to port `0x80`. Port `0x80` is the traditional
POST diagnostic port; an I/O write to it takes long enough for the older 8259A
to latch the previous command. On modern hardware and QEMU a short delay is
still the conventional way to satisfy the controller's timing.

## 4.3 Initialization sequence

```c
void pic_remap(void)
{
    /* ICW1: begin initialization, expect ICW4. */
    outb(PIC1_COMMAND, 0x11);
    io_wait();
    outb(PIC2_COMMAND, 0x11);
    io_wait();

    /* ICW2: vector offsets. */
    outb(PIC1_DATA, 0x20);
    io_wait();
    outb(PIC2_DATA, 0x28);
    io_wait();

    /* ICW3: master has the slave on IRQ2. */
    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();

    /* ICW4: 8086 mode. */
    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    /* Mask everything; drivers unmask what they need. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}
```

### ICW1 — start the initialization

`0x11 = 0001_0001b`:

| Bit | Meaning |
|---|---|
| 4 | 1 = initialization command (ICW1) |
| 3 | 0 = ADI (call address interval; ignored in 8086 mode) |
| 2 | 0 = LTIM: edge-triggered (1 would be level-triggered) |
| 1 | 0 = SNGL: cascade mode (two controllers); 1 would mean a single 8259A |
| 0 | 1 = ICW4 will be sent |

So the controllers are put into cascade mode, edge-triggered, and told to
expect a fourth initialization word. Both master and slave receive it.

### ICW2 — vector base

The master gets `0x20`, the slave `0x28`. The low three bits of each value are
ignored by the 8259A (the IRQ number supplies them), so:

* master IRQ0–IRQ7 → vectors `0x20`–`0x27`;
* slave IRQ8–IRQ15 → vectors `0x28`–`0x2F`.

### ICW3 — cascade wiring

On the master, a `1` bit marks an input connected to a slave. `0x04` sets bit
2, meaning IRQ2 is the cascade line. On the slave, the value is the slave's
cascade identity: `0x02` means "I am connected to master IRQ2". This matches
the standard PC/AT wiring.

### ICW4 — mode

`0x01` selects 8086/8088 mode (as opposed to MCS-80/85 mode), normal EOI
(not automatic), and non-buffered mode. The other bits (special fully nested
mode, buffered mode, auto-EOI) are zero.

### OCW1 — mask everything

Writing `0xFF` to each data port sets all eight mask bits. A set bit means
"masked/disabled". The kernel starts with every IRQ off and unmasks only what
it needs, which prevents unhandled devices from flooding the CPU before
handlers are registered.

## 4.4 Unmasking one IRQ

```c
void pic_unmask(uint8_t irq)
{
    uint16_t port;

    if (irq >= 16)
        return;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq = (uint8_t)(irq - 8);
        /* Cascade IRQ2 must be enabled for the slave to reach the CPU. */
        outb(PIC1_DATA, (uint8_t)(inb(PIC1_DATA) & ~(1u << 2)));
    }
    outb(port, (uint8_t)(inb(port) & ~(1u << irq)));
}
```

Step by step for an IRQ on the master (`irq < 8`):

1. Select the master data port (`0x21`).
2. Read the current mask, clear bit `irq`, write it back.

For an IRQ on the slave (`irq >= 8`):

1. Select the slave data port (`0xA1`).
2. Rebase `irq` to 0–7 within the slave.
3. Clear bit 2 in the **master** mask, enabling the cascade line. Without
   this, the slave's output never reaches the CPU.
4. Clear the chosen bit in the slave mask.

The read-modify-write is not atomic, but `pic_unmask` is only called during
single-threaded initialization (`kernel_main` calls `pic_unmask(1)`). It would
need interrupt disabling if called concurrently with another mask update.

Note a subtlety: unmasking IRQ2 directly (the cascade line) is allowed and
does not automatically unmask any slave IRQ. Conversely, unmasking a slave IRQ
always unmasks the cascade. There is no corresponding "mask cascade when the
last slave IRQ is masked" logic; once enabled, IRQ2 stays enabled.

## 4.5 End of interrupt

```c
void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_COMMAND, PIC_EOI);
    outb(PIC1_COMMAND, PIC_EOI);
}
```

`PIC_EOI` is `0x20`, the non-specific EOI command (OCW2 with `EOI=1`). It tells
the controller to clear the highest-priority in-service bit.

Order matters: for a slave IRQ, send EOI to the slave first, then to the
master. The slave's EOI clears its in-service bit; the master's EOI clears the
cascade (IRQ2) in-service bit. Sending only the master EOI would leave the
slave thinking IRQ8–15 are still in service; sending only the slave EOI would
leave the master's IRQ2 in service and block lower-priority IRQs.

Every IRQ path in `isr_dispatch` calls `pic_send_eoi(irq)`, including IRQs with
no registered handler. That keeps the controller from wedging.

## 4.6 The full IRQ path

```
 device asserts IRQ line
        |
        v
 8259A priority resolver
        |
        | INT to CPU, vector = base + IRQ number
        v
 CPU reads IDT[vector], clears IF (interrupt gate)
        |
        v
 isr stub -> isr_common -> isr_dispatch
        |
        | irq_handlers[irq]()
        v
 pic_send_eoi(irq):
   if irq >= 8: outb(0xA0, 0x20)
   outb(0x20, 0x20)
        |
        v
 iret restores IF
```

## 4.7 Priority and masking semantics

* IRQ0 has the highest priority on the master, IRQ7 the lowest. On the slave,
  IRQ8 is highest, IRQ15 lowest. Because the slave is attached to IRQ2, all
  slave IRQs effectively sit at the master's IRQ2 priority.
* A mask bit of 1 blocks the input. The mask register is independent of the
  in-service register.
* While an interrupt is in service and before EOI, the 8259A blocks the same
  and lower-priority IRQs. bnOS also has `IF=0` during the handler, so this
  matters only for the PIC's internal state.
* Edge-triggered mode means a short pulse that occurs while the line is masked
  can be lost; the keyboard controller latches its own output, so this is not
  a problem in practice.

## 4.8 What is not implemented

* **Spurious IRQ7/IRQ15 handling.** A spurious IRQ7 can occur when the
  controller raises `INT` just as the CPU acknowledges another interrupt. The
  canonical handler reads the In-Service Register (OCW3) and, if IRQ7 is not
  in service, returns without sending EOI. bnOS always sends EOI. On QEMU this
  is harmless; on some real chipsets it can cause a lost IRQ.
* **Automatic EOI.** ICW4 uses normal EOI, so software must acknowledge. This
  is deliberate: it makes the handler's completion point explicit.
* **Specific EOI.** OCW2 non-specific EOI is sufficient because there is no
  nesting.
* **IRQ priorities / rotation.** No special mask mode, no rotating priority.
* **APIC / IOAPIC / MSI.** The kernel uses the legacy PIC only. A machine
  booted with the IOAPIC enabled still has the 8259A in virtual-wire mode for
  the legacy interrupts, which is enough for the keyboard.
* **The timer (IRQ0).** It stays masked. There is no scheduler, no sleep and no
  uptime clock. The shell is purely interrupt-driven by the keyboard.
* **The RTC (IRQ8), serial (IRQ3/4), disk (IRQ14/15), mouse (IRQ12).** All
  masked. No drivers exist.

## 4.9 Why the keyboard works with so little setup

The 8042 keyboard controller is enabled by the firmware and generates IRQ1 on
every scancode. `keyboard_init` only drains stale bytes and registers the
handler; it does not send controller commands such as "enable keyboard
interface" (`0xAE`) or "enable scanning" (`0xF4`). This relies on the firmware
(or QEMU) having left the controller enabled. On unusual hardware, the keyboard
could stay silent until those commands are issued. The driver also does not set
the keyboard LEDs or select scancode set 1 explicitly; it assumes the
firmware's default set 1.

## 4.10 Initialization order constraint

`pic_remap` must run **after** `idt_init` (so the new vectors have gates) and
**before** any IRQ is unmasked. It also must run before `keyboard_init` only
for logical reasons: registering the handler does not enable the IRQ. The
actual enabling happens at the end of `kernel_main`:

```c
pic_unmask(1);
interrupts_enable();
shell_run();
```

`sti` is the last step. Before it, no maskable interrupt can be delivered even
if the PIC has one pending. After `sti`, a pending IRQ1 (if the user pressed a
key during boot) is delivered before the first shell prompt, and the byte waits
in the ring buffer.

Next: [05-keyboard.md](05-keyboard.md).
