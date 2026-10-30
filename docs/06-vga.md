# 6. VGA text console

Files: `src/vga.c`, `src/vga.h`.

The console is an 80×25 array of 16-bit cells at physical address `0xB8000`.
Each cell stores one character and one attribute byte. The driver implements
clearing, colours, the hardware cursor, scrolling, character output and a small
`printf`. It is the only output device in the kernel.

## 6.1 Memory layout

Text mode memory is 80 × 25 × 2 = 4000 bytes:

```
0xB8000  +-------------------+  cell (0,0)
         | char  | attribute |  low byte = ASCII, high byte = colour
         +-------------------+
0xB8002  | char  | attribute |  cell (1,0)
         +-------------------+
         ...
0xB8F9E  | char  | attribute |  cell (79,24)
         +-------------------+
```

The driver accesses it as `volatile uint16_t *`:

```c
#define VGA_MEMORY 0x000B8000u

static volatile uint16_t *const vga = (volatile uint16_t *)VGA_MEMORY;
```

`volatile` is required: without it the compiler could cache or eliminate writes
to a memory region it does not know is hardware-visible. The memory is normal
RAM as far as the CPU is concerned, but the VGA controller scans it
continuously. Writes appear on screen without any explicit flush.

The address is valid before paging (physical) and after paging (identity map).
`paging_init` deliberately maps at least the low 1 MiB, which covers
`0xB8000`.

## 6.2 Cell encoding

```c
static uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg)
{
    return (uint8_t)((uint8_t)fg | ((uint8_t)bg << 4));
}

static uint16_t vga_entry(char c, uint8_t color)
{
    return (uint16_t)(uint8_t)c | ((uint16_t)color << 8);
}
```

The attribute byte is:

```
 bit  7  6 5 4  3  2 1 0
     BL B B B  I  F F F
```

* bits 0–2: foreground colour (0–7);
* bit 3: foreground intensity (bright);
* bits 4–6: background colour (0–7);
* bit 7: blink (or bright background, depending on the VGA configuration).

`enum vga_color` defines the standard 16-colour palette:

| Value | Name | Value | Name |
|---|---|---|---|
| 0 | black | 8 | dark grey |
| 1 | blue | 9 | light blue |
| 2 | green | 10 | light green |
| 3 | cyan | 11 | light cyan |
| 4 | red | 12 | light red |
| 5 | magenta | 13 | light magenta |
| 6 | brown | 14 | yellow |
| 7 | light grey | 15 | white |

`vga_entry` packs the character in the low byte and the attribute in the
high byte, matching the little-endian `uint16_t` layout. For example, `'A'`
with light grey on black is `0x0741`.

## 6.3 State

```c
static uint8_t term_color;
static size_t term_row;
static size_t term_col;
```

* `term_color` is the current attribute byte. It changes only via
  `vga_set_color`.
* `term_row`/`term_col` track the write position, not the hardware cursor.
  The hardware cursor is reprogrammed after every character.

`vga_clear` initialises the state:

```c
void vga_clear(void)
{
    term_color = vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK);
    term_row = 0;
    term_col = 0;
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        vga[i] = vga_entry(' ', term_color);
    vga_move_cursor();
}
```

It fills all 2000 cells with spaces in the default colour and moves the cursor
to the top-left. Note that it **resets the colour**; the shell's prompt sets its
own colours on every iteration.

## 6.4 Hardware cursor

```c
static void vga_move_cursor(void)
{
    uint16_t pos = (uint16_t)(term_row * VGA_WIDTH + term_col);

    outb(VGA_CRTC_INDEX, 0x0F);
    outb(VGA_CRTC_DATA, (uint8_t)(pos & 0xFF));
    outb(VGA_CRTC_INDEX, 0x0E);
    outb(VGA_CRTC_DATA, (uint8_t)((pos >> 8) & 0xFF));
}
```

The CRT controller (CRTC) is accessed through an index/data pair:

* port `0x3D4` = index register;
* port `0x3D5` = data register.

Register `0x0F` is the cursor location low byte; register `0x0E` is the high
byte. The position is a linear cell index `row * 80 + col`, not a byte offset.
The CRTC expects 16 bits, split into two 8-bit writes.

This is called after every `vga_putc`, even for control characters, so the
blinking cursor always matches `term_row`/`term_col`. It costs four port writes
per character. A faster implementation would only update the cursor when the
position actually changes or batch output, but the shell is interactive and
this keeps the code simple.

## 6.5 Newline and scrolling

```c
static void vga_newline(void)
{
    term_col = 0;
    if (++term_row == VGA_HEIGHT) {
        /* Scroll up one row in place. */
        for (size_t i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++)
            vga[i] = vga[i + VGA_WIDTH];
        for (size_t i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++)
            vga[i] = vga_entry(' ', term_color);
        term_row = VGA_HEIGHT - 1;
    }
}
```

Step by step:

1. Set the column to 0.
2. Increment the row. If it is still less than 25, nothing else happens.
3. If the row reached 25, the screen is full:
   * copy cells `[80, 2000)` to `[0, 1920)`, i.e. move rows 1–24 up by one row;
   * fill the last row `[1920, 2000)` with spaces in the current colour;
   * keep `term_row = 24`.
4. `term_col` is already 0.

The copy is forward (`i` increasing, source `i + 80`), which is safe because the
source is always ahead of the destination; no cell is overwritten before it is
read. There is no hardware scroll and no double buffering.

Because the blanking uses `term_color`, a scrolled-in line inherits the colour
that was active at the moment of the newline. Since the shell writes whole
lines and then changes colour for the prompt, this is visually correct.

## 6.6 Character output

```c
void vga_putc(char c)
{
    switch (c) {
    case '\n':
        vga_newline();
        break;
    case '\r':
        term_col = 0;
        break;
    case '\t':
        term_col = (term_col + 8) & ~(size_t)7;
        if (term_col >= VGA_WIDTH)
            vga_newline();
        break;
    case '\b':
        if (term_col > 0) {
            term_col--;
            vga[term_row * VGA_WIDTH + term_col] = vga_entry(' ', term_color);
        }
        break;
    default:
        vga[term_row * VGA_WIDTH + term_col] = vga_entry(c, term_color);
        if (++term_col == VGA_WIDTH)
            vga_newline();
        break;
    }
    vga_move_cursor();
}
```

Behaviour by character:

| Character | Effect |
|---|---|
| `'\n'` | newline, scrolling if needed |
| `'\r'` | column = 0, same row |
| `'\t'` | advance to the next multiple of 8; wrap if beyond column 79 |
| `'\b'` | if column > 0, move left and write a space over the previous cell |
| anything else | write the character at the current position, advance one column |

Details:

* Tab stops are at columns 0, 8, 16, …, 72. From column 73, tab wraps to the
  next line (because `(73 + 8) & ~7 = 80`, which is `>= 80`).
* Backspace only erases within the current row. At column 0 it does nothing;
  it does not move to the end of the previous line. The shell never calls it
  at column 0 because it checks `line_length > 0`.
* Wrapping happens after writing column 79: the character is stored, then
  `vga_newline` moves to the next row. There is no pending-wrap state; the
  cursor never sits "one past the end" of a row.
* Control characters other than the four above fall into `default` and are
  written as cells. The shell filters input to printable ASCII, and the kernel's
  own strings do not contain control characters other than `\n`. A NUL would
  produce a blank cell with the current attribute.
* The default branch does not handle UTF-8. Each byte is one cell; the VGA text
  font is code page 437, so only ASCII renders predictably.

## 6.7 String output

```c
void vga_write(const char *s)
{
    if (s == NULL)
        s = "(null)";
    while (*s != '\0')
        vga_putc(*s++);
}
```

`vga_write` is a thin loop. It is the only function that tolerates a null
pointer. It stops at the first NUL; there is no length parameter.

## 6.8 The custom `printf`

`vga_printf` is a small variadic formatter, not a libc `printf`:

```c
void vga_printf(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            vga_putc(*p);
            continue;
        }

        p++;
        switch (*p) {
        case '%': vga_putc('%'); break;
        case 'c': vga_putc((char)va_arg(args, int)); break;
        case 's': vga_write(va_arg(args, const char *)); break;
        case 'd':
        case 'i': vga_put_int(va_arg(args, int)); break;
        case 'u': vga_put_uint(va_arg(args, unsigned int), 10, 0); break;
        case 'x': vga_put_uint(va_arg(args, unsigned int), 16, 0); break;
        case 'X': vga_put_uint(va_arg(args, unsigned int), 16, 1); break;
        case 'p': { ... print 0x + eight hex digits ... } break;
        case '\0': p--; break;
        default: vga_putc('%'); vga_putc(*p); break;
        }
    }
    va_end(args);
}
```

Supported conversions:

| Specifier | Argument type | Output |
|---|---|---|
| `%%` | — | literal `%` |
| `%c` | `int` (promoted `char`) | one character |
| `%s` | `const char *` | string, `(null)` for NULL |
| `%d`, `%i` | `int` | signed decimal |
| `%u` | `unsigned int` | unsigned decimal |
| `%x` | `unsigned int` | lowercase hex, no leading zeros |
| `%X` | `unsigned int` | uppercase hex, no leading zeros |
| `%p` | `void *` | `0x` followed by exactly eight lowercase hex digits |

Everything else is unsupported. In particular:

* no width, precision, `+`, space, `#` or `0` flags;
* no length modifiers (`%l`, `%ll`, `%z`, `%h`);
* no `%o`, `%b`, `%f`, `%e`, `%g`;
* no `%n` (good: it is dangerous).

Unknown conversions are printed literally: `%q` outputs `%q`. A trailing lone
`%` is handled by decrementing `p` so the loop's `p++` lands on the NUL and
terminates; without that, the loop would read past the end of the string.

### Printing unsigned numbers

```c
static void vga_put_uint(uint32_t value, uint32_t base, int uppercase)
{
    static const char lower[] = "0123456789abcdef";
    static const char upper[] = "0123456789ABCDEF";
    const char *digits = uppercase ? upper : lower;
    char buf[32];
    size_t len = 0;

    do {
        buf[len++] = digits[value % base];
        value /= base;
    } while (value != 0);

    while (len > 0)
        vga_putc(buf[--len]);
}
```

The digits are generated least-significant first into a local buffer and then
emitted in reverse. The `do/while` guarantees that zero prints as `"0"` rather
than an empty string. `buf[32]` is more than enough for base 10 (10 digits) and
base 16 (8 digits).

### Printing signed numbers

```c
static void vga_put_int(int value)
{
    if (value < 0) {
        vga_putc('-');
        vga_put_uint(0u - (uint32_t)value, 10, 0);
    } else {
        vga_put_uint((uint32_t)value, 10, 0);
    }
}
```

The negation is done in unsigned arithmetic to avoid signed overflow for
`INT_MIN` (`-2147483648`). `0u - (uint32_t)INT_MIN` is `2147483648`, which
prints correctly after the minus sign.

### Printing pointers

```c
case 'p': {
    uint32_t value = (uint32_t)(uintptr_t)va_arg(args, void *);
    vga_write("0x");
    for (int shift = 28; shift >= 0; shift -= 4)
        vga_putc("0123456789abcdef"[(value >> shift) & 0xF]);
    break;
}
```

Pointers are always eight hex digits, zero-padded. This is convenient for
kernel addresses (`0x00100000`) and differs from glibc's `%p`, which may omit
leading zeros. On i386, `void *` and `uint32_t` are the same width; the
`uintptr_t` cast makes the intent explicit.

### Variadic argument promotion

`va_arg(args, int)` for `%c` is correct because a `char` passed through `...`
is promoted to `int`. `%s` reads a pointer, `%p` reads a `void *`, and the
integer conversions read `int` or `unsigned int`. Passing the wrong type is
undefined behaviour; the compiler does not check the format string because
there is no `__attribute__((format))` annotation.

## 6.9 Colour management

```c
void vga_set_color(enum vga_color fg, enum vga_color bg)
{
    term_color = vga_entry_color(fg, bg);
}
```

The colour is global and persists until changed. The kernel uses:

* light cyan on black for the banner;
* dark grey on black for separators;
* light grey on black for normal text;
* light red on black for exceptions;
* light green on black for the shell prompt.

Background colours above 7 set the blink bit through `bg << 4`; the hardware
may blink the cell instead of showing a bright background. The kernel never
uses such a background.

## 6.10 Concurrency

All output happens in one context at a time:

* during boot, `kernel_main` writes with interrupts disabled (before `sti`);
* at run time, the shell writes with interrupts enabled, but the only IRQ
  handler (`keyboard_irq`) does not write to the VGA;
* exception handlers write after disabling interrupts and then halt.

There is therefore no lock around the cursor state or the VGA buffer. If a
future IRQ handler printed to the console, it could interleave with the shell
and corrupt `term_row`/`term_col`. A real kernel would use a spinlock or defer
console output.

## 6.11 What is missing

* **No mode setting.** The kernel assumes the firmware left the machine in
  80×25 16-colour text mode. It does not program the VGA registers or use
  VBE. `qemu -kernel` and GRUB both leave text mode active.
* **No font loading.** The code page 437 font in the VGA ROM is used as-is.
* **No Unicode/UTF-8.** Bytes map directly to glyphs.
* **No ANSI escape sequences.** Colours are set through C calls, not `\033[...m`.
* **No scrolling region, no hardware cursor shape control, no blinking
  configuration.**
* **No double buffering.** Every character is written straight to display
  memory; scrolling is a 1920-cell copy.
* **No output rate limiting.** Each character costs four cursor port writes.
  Printing a large table is slow but correct.
* **No `%f`.** Floating point is not used anywhere in the kernel.

Next: [07-pmm.md](07-pmm.md).
