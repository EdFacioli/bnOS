/*
 * Mini shell: line editing, tokenizing and a small command table.
 * Input is blocking on the keyboard ring buffer; the CPU halts while idle.
 */

#include "shell.h"

#include "keyboard.h"
#include "lib.h"
#include "paging.h"
#include "pmm.h"
#include "vga.h"

#include <stdint.h>

#define LINE_MAX 128
#define ARGS_MAX 8

typedef void (*command_fn)(int argc, char **argv);

struct command {
    const char *name;
    const char *description;
    command_fn run;
};

static char line_buffer[LINE_MAX];
static int line_length;
static uint32_t last_alloc;

static void cmd_help(int argc, char **argv);
static void cmd_clear(int argc, char **argv);
static void cmd_echo(int argc, char **argv);
static void cmd_mem(int argc, char **argv);
static void cmd_paging(int argc, char **argv);
static void cmd_vmtest(int argc, char **argv);
static void cmd_alloc(int argc, char **argv);
static void cmd_free(int argc, char **argv);
static void cmd_about(int argc, char **argv);
static void cmd_halt(int argc, char **argv);

static const struct command commands[] = {
    { "help",  "list available commands", cmd_help },
    { "clear", "clear the screen", cmd_clear },
    { "echo",  "print arguments", cmd_echo },
    { "mem",   "physical memory statistics", cmd_mem },
    { "paging", "paging status and mapped pages", cmd_paging },
    { "vmtest", "map a frame at 0xC0000000 and verify it", cmd_vmtest },
    { "alloc", "allocate one 4 KiB page", cmd_alloc },
    { "free",  "free the last page allocated (or: free <addr>)", cmd_free },
    { "about", "kernel build information", cmd_about },
    { "halt",  "stop the CPU", cmd_halt },
};

#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

static void shell_prompt(void)
{
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_write("bnos> ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void read_line(void)
{
    line_length = 0;

    for (;;) {
        char c = keyboard_getchar();

        if (c == '\n') {
            vga_putc('\n');
            line_buffer[line_length] = '\0';
            return;
        }
        if (c == '\b') {
            if (line_length > 0) {
                line_length--;
                vga_putc('\b');
            }
            continue;
        }
        if (c >= ' ' && c <= '~' && line_length < LINE_MAX - 1) {
            line_buffer[line_length++] = c;
            vga_putc(c);
        }
    }
}

static int tokenize(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;

    while (*p != '\0') {
        while (*p == ' ')
            p++;
        if (*p == '\0')
            break;
        if (argc < max)
            argv[argc++] = p;
        while (*p != '\0' && *p != ' ')
            p++;
        if (*p != '\0')
            *p++ = '\0';
    }
    return argc;
}

static void execute(char *line)
{
    char *argv[ARGS_MAX];
    int argc = tokenize(line, argv, ARGS_MAX);

    if (argc == 0)
        return;

    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            commands[i].run(argc, argv);
            return;
        }
    }

    vga_printf("unknown command: %s (try 'help')\n", argv[0]);
}

static void cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    vga_write("commands:\n");
    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        vga_write("  ");
        vga_write(commands[i].name);
        vga_write(" - ");
        vga_write(commands[i].description);
        vga_putc('\n');
    }
}

static void cmd_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    vga_clear();
}

static void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            vga_putc(' ');
        vga_write(argv[i]);
    }
    vga_putc('\n');
}

static void cmd_mem(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uint32_t total = pmm_total_pages();
    uint32_t free_now = pmm_free_pages();
    uint32_t used = total - free_now;

    vga_printf("frames : total=%u free=%u used=%u (4 KiB each)\n",
               total, free_now, used);
    vga_printf("memory : total=%u MiB free=%u MiB used=%u MiB\n",
               total / 256, free_now / 256, used / 256);
}

static void cmd_paging(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    vga_printf("paging : %s (identity map, 4 KiB pages)\n",
               paging_enabled() ? "enabled" : "disabled");
    vga_printf("CR3    : 0x%X (page directory)\n", paging_directory_address());
    vga_printf("mapped : %u pages (%u MiB)\n",
               paging_mapped_pages(), paging_mapped_pages() / 256);
}

#define VMTEST_ADDRESS 0xC0000000u

static void cmd_vmtest(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (paging_is_mapped(VMTEST_ADDRESS)) {
        vga_write("vmtest: 0xC0000000 is already mapped, skipped\n");
        return;
    }

    void *frame = pmm_alloc_page();
    if (frame == 0) {
        vga_write("vmtest: out of physical memory\n");
        return;
    }

    uint32_t phys = (uint32_t)(uintptr_t)frame;
    if (!paging_map_page(VMTEST_ADDRESS, phys, 1)) {
        pmm_free_page(frame);
        vga_write("vmtest: could not map page\n");
        return;
    }

    volatile uint32_t *mapped = (volatile uint32_t *)(uintptr_t)VMTEST_ADDRESS;
    *mapped = 0xDEADBEEFu;
    uint32_t readback = *mapped;

    paging_unmap_page(VMTEST_ADDRESS);
    pmm_free_page(frame);

    vga_printf("vmtest: phys 0x%X -> virt 0x%X, wrote 0xDEADBEEF, read 0x%X\n",
               phys, VMTEST_ADDRESS, readback);
    vga_printf("result: %s (unmapped and frame freed)\n",
               readback == 0xDEADBEEFu ? "ok" : "FAILED");
}

static void cmd_alloc(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    void *page = pmm_alloc_page();
    if (page == 0) {
        vga_write("out of physical memory\n");
        return;
    }

    last_alloc = (uint32_t)(uintptr_t)page;
    vga_printf("allocated 4 KiB at 0x%X\n", last_alloc);
}

static void cmd_free(int argc, char **argv)
{
    uint32_t address = last_alloc;

    if (argc > 1) {
        char *end = 0;
        address = (uint32_t)strtoul(argv[1], &end, 0);
        if (end == argv[1] || (end != 0 && *end != '\0')) {
            vga_printf("invalid address: %s\n", argv[1]);
            return;
        }
    }

    if (address == 0) {
        vga_write("no page to free (use 'alloc' first)\n");
        return;
    }

    if (!pmm_free_page((void *)(uintptr_t)address)) {
        vga_printf("0x%X is not an allocated page\n", address);
        return;
    }

    vga_printf("freed 4 KiB page 0x%X\n", address);
    if (address == last_alloc)
        last_alloc = 0;
}

static void cmd_about(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    vga_write("bnOS - 32-bit x86 kernel\n");
    vga_write("GDT (flat), IDT (48 gates), 8259A PIC, PS/2 keyboard IRQ1,\n");
    vga_write("bitmap physical memory manager, identity paging + map/unmap,\n");
    vga_write("VGA text console.\n");
}

static void cmd_halt(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    vga_write("CPU halted.\n");
    for (;;)
        __asm__ volatile("cli; hlt");
}

void shell_run(void)
{
    vga_write("Type 'help' for available commands.\n");

    for (;;) {
        shell_prompt();
        read_line();
        execute(line_buffer);
    }
}
