# 9. The shell

Files: `src/shell.c`, `src/shell.h`.

The shell is a single-threaded, blocking command loop. It reads a line from the
keyboard ring buffer, echoes it to VGA, splits it into whitespace-separated
tokens, looks the first token up in a static command table, and calls the
corresponding function. There are no processes, no job control, no pipes, no
redirection and no history.

## 9.1 Data structures

```c
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
```

* `line_buffer` holds the current line, including the NUL terminator.
  `LINE_MAX - 1 = 127` printable characters fit.
* `line_length` is the number of characters currently in the buffer.
* `last_alloc` remembers the most recent frame returned by `alloc`, so
  `free` with no argument can release it.
* `command_fn` is a plain function pointer taking `(argc, argv)` like a
  conventional C `main`.

The table:

```c
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
```

The table is `const` and lives in `.rodata`. `COMMAND_COUNT` is computed by the
compiler, so adding or removing an entry automatically updates `help` and the
lookup loop.

## 9.2 The main loop

```c
void shell_run(void)
{
    vga_write("Type 'help' for available commands.\n");

    for (;;) {
        shell_prompt();
        read_line();
        execute(line_buffer);
    }
}
```

`shell_run` never returns. Each iteration:

1. Prints the prompt in light green on black:
   ```c
   static void shell_prompt(void)
   {
       vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
       vga_write("bnos> ");
       vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
   }
   ```
2. Blocks in `read_line` until Enter.
3. Executes the line.

The prompt colour is reset to light grey immediately, so echoed input and
command output use the normal colour. `clear` resets the colour to light grey
too; the next prompt sets green again.

## 9.3 Reading a line

```c
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
```

Step by step:

* Reset the length. The buffer's old contents are irrelevant because the first
  character overwrites them and the NUL is written at the end.
* `keyboard_getchar()` blocks until a character is available. It uses `sti;
  hlt` internally when the ring buffer is empty.
* **Enter** (`'\n'`): print a newline, terminate the string at the current
  length, return. The NUL always fits because the length is capped at 127.
* **Backspace** (`'\b'`): if there is at least one character, decrement the
  length and call `vga_putc('\b')`. `vga_putc` moves the cursor back and writes
  a space over the old character. At column 0 it does nothing, and the length
  is not decremented, so the prompt cannot be erased.
* **Printable ASCII** (`' '` through `'~'`): append and echo, provided the
  buffer is not full. At 127 characters, further printable input is silently
  ignored; the user must backspace before adding more.
* Everything else (Tab, Ctrl characters, extended keys that the keyboard
  driver happens to translate) is ignored.

`read_line` does not echo the NUL and does not print a prompt again. It assumes
the prompt has already been printed and the cursor is on the same line.

Note the line editing is minimal: there is no cursor movement within the line,
no insert/delete, no history, no Ctrl-U/Ctrl-W, and no paste handling beyond
the 256-byte keyboard buffer. Backspace only works at the end of the line.

## 9.4 Tokenizing in place

```c
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
```

This is a destructive tokenizer: it replaces spaces with NUL bytes, so
`line_buffer` is chopped into C strings.

Walkthrough for `"  echo  hello   world "`:

1. Skip leading spaces.
2. `argv[0] = "echo"` (pointer to `e`).
3. Scan to the next space; replace it with `'\0'`, advance past it.
4. Skip the second space.
5. `argv[1] = "hello"`, terminate.
6. Skip the three spaces.
7. `argv[2] = "world"`, terminate at the space.
8. Skip the final space; `*p == '\0'`; stop.
9. `argc = 3`.

Properties:

* Only the ASCII space `' '` separates tokens. Tabs are not separators. The
  keyboard driver maps Tab to `'\t'`, but `read_line` rejects it (`'\t' < ' '`),
  so tabs cannot reach the tokenizer.
* Runs of spaces are collapsed.
* Empty input (`argc = 0`) is handled by `execute` returning immediately.
* `argc` is capped at `max` (8). If the line has more than 8 tokens, the extra
  tokens are still scanned and NUL-terminated, but their pointers are not
  stored. The command only sees the first 8.
* There is no quoting, escaping or comment syntax. `echo "a b"` prints
  `"a b"` including the quotes, split into two tokens.
* The function never writes past `line`'s NUL; it only replaces spaces.

## 9.5 Command lookup

```c
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
```

* Tokenize into at most 8 arguments.
* Empty line → no output, prompt again.
* Linear search with exact, case-sensitive `strcmp`. `Help`, `HELP` and `hel`
  are all unknown.
* The first match wins. If two commands had the same name, the earlier entry
  would shadow the later one.
* Unknown command prints the offending token and a hint.
* Command functions receive `argc` and `argv`; most ignore both with
  `(void)argc; (void)argv;` to satisfy `-Wall -Wextra`.
* Commands run with interrupts enabled. The keyboard IRQ can fill the ring
  buffer while a command runs, but commands do not read the keyboard, so input
  typed during a long command is queued for the next line (up to 256 bytes).

## 9.6 The commands

### `help`

```c
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
```

Walks the table in order. Because the table is the single source of truth,
`help` cannot drift out of sync with the dispatch logic.

### `clear`

```c
static void cmd_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    vga_clear();
}
```

Clears the screen, resets the colour to light grey on black, and moves the
cursor to `(0,0)`. The prompt is printed by the next loop iteration.

### `echo`

```c
static void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            vga_putc(' ');
        vga_write(argv[i]);
    }
    vga_putc('\n');
}
```

Prints arguments 1..`argc-1` separated by single spaces, then a newline.
Multiple spaces in the input collapse to one. `echo` with no arguments prints
just a newline.

### `mem`

```c
static void cmd_mem(int argc, char **argv)
{
    uint32_t total = pmm_total_pages();
    uint32_t free_now = pmm_free_pages();
    uint32_t used = total - free_now;

    vga_printf("frames : total=%u free=%u used=%u (4 KiB each)\n",
               total, free_now, used);
    vga_printf("memory : total=%u MiB free=%u MiB used=%u MiB\n",
               total / 256, free_now / 256, used / 256);
}
```

* `total` is the highest usable frame count, not installed RAM.
* `used = total - free` includes low memory, MMIO holes and the kernel.
* 256 frames = 1 MiB, hence the `/ 256` conversion. Integer division truncates,
  so a value of 1.5 MiB prints as `1 MiB`.
* The command does not allocate or free anything.

### `paging`

```c
static void cmd_paging(int argc, char **argv)
{
    vga_printf("paging : %s (identity map, 4 KiB pages)\n",
               paging_enabled() ? "enabled" : "disabled");
    vga_printf("CR3    : 0x%X (page directory)\n", paging_directory_address());
    vga_printf("mapped : %u pages (%u MiB)\n",
               paging_mapped_pages(), paging_mapped_pages() / 256);
}
```

* `paging_enabled()` reads CR0.PG.
* `paging_directory_address()` reads CR3 (the physical page-directory
  address).
* `paging_mapped_pages()` returns the count of present PTEs, including the
  identity map and any `vmtest` mapping. It does not count page tables
  themselves.
* The `%X` prints uppercase hex with no leading zeros, so CR3 looks like
  `0x14A000`, not `0x0014A000`.

### `vmtest`

Covered in detail in `08-paging.md`. Summary:

* Skips if `0xC0000000` is already mapped.
* Allocates a frame, maps it at `0xC0000000`, writes `0xDEADBEEF`, reads back.
* Unmaps and frees the frame.
* Prints the physical address, virtual address and readback, then `ok` or
  `FAILED`.
* Leaks the page table created for PDE 768 the first time it runs.

### `alloc`

```c
static void cmd_alloc(int argc, char **argv)
{
    void *page = pmm_alloc_page();
    if (page == 0) {
        vga_write("out of physical memory\n");
        return;
    }

    last_alloc = (uint32_t)(uintptr_t)page;
    vga_printf("allocated 4 KiB at 0x%X\n", last_alloc);
}
```

* Allocates one frame with `pmm_alloc_page` (freeable).
* Stores its address in `last_alloc`, replacing the previous value.
* Prints the physical address.
* The frame is **not** zeroed and **not** mapped at a new virtual address; it
  is already accessible through the identity map.
* Repeated `alloc` calls leak every frame except the most recent one in terms
  of the shell's tracking. You can still free older frames by passing their
  address explicitly: `free 0x...`.

### `free`

```c
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
```

Step by step:

1. Default to `last_alloc`.
2. If an argument is present, parse it with `strtoul(..., base 0)`:
   * `0x...` → hexadecimal;
   * leading `0` → octal;
   * otherwise decimal.
3. Validate that the entire token was consumed. `end == argv[1]` means no
   digits were parsed; `*end != '\0'` means trailing garbage. Both print
   `invalid address`.
4. Reject `0` (no remembered allocation, or the user explicitly passed 0).
5. Call `pmm_free_page`. It enforces alignment, range and the
   `alloc_bitmap`, so reserved/kernel/double-freed frames are rejected with a
   message.
6. On success, clear `last_alloc` only if the freed address was the remembered
   one. Freeing an older explicit address leaves `last_alloc` intact.

Examples:

```
bnos> alloc
allocated 4 KiB at 0x14C000
bnos> free
freed 4 KiB page 0x14C000
bnos> free 0x14C000
0x14C000 is not an allocated page
bnos> free 0x100000
0x100000 is not an allocated page
```

### `about`

```c
static void cmd_about(int argc, char **argv)
{
    vga_write("bnOS - 32-bit x86 kernel\n");
    vga_write("GDT (flat), IDT (48 gates), 8259A PIC, PS/2 keyboard IRQ1,\n");
    vga_write("bitmap physical memory manager, identity paging + map/unmap,\n");
    vga_write("VGA text console.\n");
}
```

Static description of the build. It does not read a version constant, so it
cannot drift from the code except by human error.

### `halt`

```c
static void cmd_halt(int argc, char **argv)
{
    vga_write("CPU halted.\n");
    for (;;)
        __asm__ volatile("cli; hlt");
}
```

Prints a message and stops the CPU permanently. `cli` prevents the keyboard IRQ
from waking it; `hlt` waits for an interrupt that cannot arrive (NMI or SMI
could still wake the CPU, but the loop immediately halts again). There is no
reboot command; reset the machine or QEMU.

## 9.7 A sample session

```
  bnOS - 32-bit x86 kernel
  ----------------------------------------------
  VGA text buffer : 0x000B8000 (80x25 char cells)
  Multiboot magic : 0x2BADB002 (valid)
  ...
Type 'help' for available commands.
bnos> help
commands:
  help - list available commands
  clear - clear the screen
  echo - print arguments
  mem - physical memory statistics
  paging - paging status and mapped pages
  vmtest - map a frame at 0xC0000000 and verify it
  alloc - allocate one 4 KiB page
  free - free the last page allocated (or: free <addr>)
  about - kernel build information
  halt - stop the CPU
bnos> echo hello world
hello world
bnos> mem
frames : total=32736 free=32371 used=365 (4 KiB each)
memory : total=127 MiB free=126 MiB used=1 MiB
bnos> paging
paging : enabled (identity map, 4 KiB pages)
CR3    : 0x14A000 (page directory)
mapped : 32736 pages (127 MiB)
bnos> vmtest
vmtest: phys 0x16D000 -> virt 0xC0000000, wrote 0xDEADBEEF, read 0xDEADBEEF
result: ok (unmapped and frame freed)
bnos> alloc
allocated 4 KiB at 0x16D000
bnos> free
freed 4 KiB page 0x16D000
bnos> about
bnOS - 32-bit x86 kernel
GDT (flat), IDT (48 gates), 8259A PIC, PS/2 keyboard IRQ1,
bitmap physical memory manager, identity paging + map/unmap,
VGA text console.
bnos> halt
CPU halted.
```

The exact numbers depend on the machine's memory map and on how many page
tables `paging_init` allocated.

## 9.8 Edge cases and limitations

* **Line length.** 127 printable characters; extra input is dropped. There is
  no bell or error message.
* **Arguments.** 8 tokens maximum; extra tokens are silently ignored.
* **Spaces only.** Tabs and other whitespace are not separators. Input is
  filtered to printable ASCII, so tabs never reach the tokenizer.
* **No quoting/escaping.** `echo "a b"` prints `"a b"`; there is no way to
  embed a space in one argument.
* **Case-sensitive.** `HELP` is unknown.
* **No history.** Arrow keys are discarded by the keyboard driver.
* **No cursor editing.** Only append and backspace at the end.
* **No Ctrl-C.** A long command cannot be interrupted. The only potentially
  long operations are `vmtest` and `mem`; both are short.
* **No background jobs, pipes or redirection.**
* **`free` address parsing.** `strtoul` accepts a leading `-` and wraps
  modulo 2^32; such an address will fail the range/bitmap checks and be
  rejected. A leading `0` is octal, which can surprise users: `free 010` means
  decimal 8, not decimal 10. Use `0x...` or a decimal without a leading zero.
* **Only one tracked allocation.** `last_alloc` holds one address; use explicit
  addresses to free older frames.
* **Commands do not validate extra arguments.** `mem foo` ignores `foo`.
* **No permissions.** Everything runs in ring 0; a command can corrupt any
  memory it can address. This is a demo kernel, not a secure shell.
* **`halt` is irreversible.** No NMI watchdog, no reboot.
* **`vmtest` on large machines.** With ≥3 GiB of managed RAM, the identity map
  already covers `0xC0000000`, so `vmtest` skips. That is a limitation of the
  chosen test address, not a paging bug.

Next: [10-lib.md](10-lib.md).
