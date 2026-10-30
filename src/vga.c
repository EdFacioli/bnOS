/*
 * VGA text mode driver: 80x25 cells at physical 0xB8000.
 * Each cell is a 16-bit little-endian word: low byte = character,
 * high byte = attribute (low nibble foreground, high nibble background).
 */

#include "vga.h"

#include "io.h"

#include <stdarg.h>

#define VGA_MEMORY 0x000B8000u
#define VGA_CRTC_INDEX 0x3D4
#define VGA_CRTC_DATA  0x3D5

static volatile uint16_t *const vga = (volatile uint16_t *)VGA_MEMORY;
static uint8_t term_color;
static size_t term_row;
static size_t term_col;

static uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg)
{
    return (uint8_t)((uint8_t)fg | ((uint8_t)bg << 4));
}

static uint16_t vga_entry(char c, uint8_t color)
{
    return (uint16_t)(uint8_t)c | ((uint16_t)color << 8);
}

/* Program the hardware text cursor to track term_row/term_col. */
static void vga_move_cursor(void)
{
    uint16_t pos = (uint16_t)(term_row * VGA_WIDTH + term_col);

    outb(VGA_CRTC_INDEX, 0x0F);
    outb(VGA_CRTC_DATA, (uint8_t)(pos & 0xFF));
    outb(VGA_CRTC_INDEX, 0x0E);
    outb(VGA_CRTC_DATA, (uint8_t)((pos >> 8) & 0xFF));
}

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

void vga_clear(void)
{
    term_color = vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK);
    term_row = 0;
    term_col = 0;
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        vga[i] = vga_entry(' ', term_color);
    vga_move_cursor();
}

void vga_set_color(enum vga_color fg, enum vga_color bg)
{
    term_color = vga_entry_color(fg, bg);
}

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

void vga_write(const char *s)
{
    if (s == NULL)
        s = "(null)";
    while (*s != '\0')
        vga_putc(*s++);
}

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

static void vga_put_int(int value)
{
    if (value < 0) {
        vga_putc('-');
        vga_put_uint(0u - (uint32_t)value, 10, 0);
    } else {
        vga_put_uint((uint32_t)value, 10, 0);
    }
}

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
        case '%':
            vga_putc('%');
            break;
        case 'c':
            vga_putc((char)va_arg(args, int));
            break;
        case 's':
            vga_write(va_arg(args, const char *));
            break;
        case 'd':
        case 'i':
            vga_put_int(va_arg(args, int));
            break;
        case 'u':
            vga_put_uint(va_arg(args, unsigned int), 10, 0);
            break;
        case 'x':
            vga_put_uint(va_arg(args, unsigned int), 16, 0);
            break;
        case 'X':
            vga_put_uint(va_arg(args, unsigned int), 16, 1);
            break;
        case 'p': {
            uint32_t value = (uint32_t)(uintptr_t)va_arg(args, void *);
            vga_write("0x");
            for (int shift = 28; shift >= 0; shift -= 4)
                vga_putc("0123456789abcdef"[(value >> shift) & 0xF]);
            break;
        }
        case '\0':
            p--; /* trailing lone '%': finish the loop */
            break;
        default:
            vga_putc('%');
            vga_putc(*p);
            break;
        }
    }
    va_end(args);
}
