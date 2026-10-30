#include "gdt.h"
#include "idt.h"
#include "io.h"
#include "keyboard.h"
#include "multiboot.h"
#include "paging.h"
#include "pic.h"
#include "pmm.h"
#include "shell.h"
#include "vga.h"

#include <stdint.h>

extern uint8_t __bss_end[];

static void halt(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}

void kernel_main(uint32_t magic, uint32_t info)
{
    int valid_mbi = (magic == MULTIBOOT_BOOTLOADER_MAGIC);
    const struct multiboot_info *mbi =
        valid_mbi ? (const struct multiboot_info *)(uintptr_t)info : 0;

    vga_clear();

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_write("  bnOS - 32-bit x86 kernel\n");

    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_write("  ----------------------------------------------\n");

    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_write("  VGA text buffer : 0x000B8000 (80x25 char cells)\n");
    vga_printf("  Multiboot magic : 0x%X %s\n", magic,
               valid_mbi ? "(valid)" : "(unexpected)");
    vga_printf("  Multiboot info  : %p\n", (void *)(uintptr_t)info);

    gdt_init();
    idt_init();
    pic_remap();
    keyboard_init();
    pmm_init(mbi, (uint32_t)(uintptr_t)__bss_end);
    paging_init((uint32_t)(uintptr_t)__bss_end);

    vga_write("  CPU mode        : protected mode, 32-bit flat\n");
    vga_write("  GDT             : flat kernel code/data loaded\n");
    vga_write("  IDT             : 32 exceptions + 16 IRQ gates\n");
    vga_write("  PIC             : remapped to 0x20..0x2F, IRQ1 ready\n");
    vga_printf("  Memory          : %u MiB total, %u pages free (%u MiB)\n",
               pmm_total_pages() / 256, pmm_free_pages(), pmm_free_pages() / 256);
    vga_printf("  Paging          : %s, identity %u MiB, CR3=0x%X\n",
               paging_enabled() ? "enabled" : "disabled",
               paging_mapped_pages() / 256, paging_directory_address());

    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_write("  ----------------------------------------------\n");

    /* From here on the keyboard IRQ and the shell's idle hlt run. */
    pic_unmask(1);
    interrupts_enable();

    shell_run();
    halt();
}
