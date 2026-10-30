#include "idt.h"

#include "pic.h"
#include "vga.h"

#include <stdint.h>

#define IDT_ENTRIES 256
#define KERNEL_CODE_SELECTOR 0x08
#define IDT_GATE_INTERRUPT 0x8E /* present, ring 0, 32-bit interrupt gate */

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

/* Must match the stack layout built by isr_common in isr.S. */
struct isr_frame {
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t vector, error;
    uint32_t eip, cs, eflags;
};

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idt_descriptor;
static irq_handler_t irq_handlers[16];

extern const uint32_t isr_stub_table[48];

static const char *const exception_names[32] = {
    "divide error", "debug", "non-maskable interrupt", "breakpoint",
    "overflow", "bound range exceeded", "invalid opcode", "device not available",
    "double fault", "coprocessor segment overrun", "invalid TSS",
    "segment not present", "stack-segment fault", "general protection fault",
    "page fault", "reserved", "x87 floating-point exception", "alignment check",
    "machine check", "SIMD floating-point exception", "virtualization exception",
    "control protection exception", "reserved", "reserved", "reserved",
    "reserved", "reserved", "reserved", "hypervisor injection exception",
    "VMM communication exception", "security exception", "reserved",
};

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

void irq_register_handler(uint8_t irq, irq_handler_t handler)
{
    if (irq < 16)
        irq_handlers[irq] = handler;
}

void isr_dispatch(struct isr_frame *frame)
{
    if (frame->vector < 32) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_printf("\nEXCEPTION %u: %s (error=0x%X) eip=%p\n",
                   (unsigned int)frame->vector,
                   exception_names[frame->vector],
                   (unsigned int)frame->error,
                   (void *)(uintptr_t)frame->eip);

        if (frame->vector == 14) {
            uint32_t fault_address;
            __asm__ volatile("mov %%cr2, %0" : "=r"(fault_address));
            vga_printf("  fault address : %p\n", (void *)(uintptr_t)fault_address);
            vga_printf("  cause         : %s, %s, %s%s%s\n",
                       (frame->error & 0x1) ? "protection violation" : "page not present",
                       (frame->error & 0x2) ? "write" : "read",
                       (frame->error & 0x4) ? "user mode" : "kernel mode",
                       (frame->error & 0x8) ? ", reserved bit set" : "",
                       (frame->error & 0x10) ? ", instruction fetch" : "");
        }

        vga_write("System halted.\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }

    uint8_t irq = (uint8_t)(frame->vector - 32);
    if (irq < 16 && irq_handlers[irq] != 0)
        irq_handlers[irq]();
    pic_send_eoi(irq);
}
