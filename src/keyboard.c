/*
 * PS/2 keyboard driver, scancode set 1 (US layout), driven by IRQ1.
 * Decoded characters go into a small ring buffer read by the shell.
 */

#include "keyboard.h"

#include "idt.h"
#include "io.h"

#include <stdint.h>

#define KEYBOARD_DATA_PORT   0x60
#define KEYBOARD_STATUS_PORT 0x64
#define KEYBOARD_BUFFER_SIZE 256

static const char keymap[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',',
    [0x34] = '.', [0x35] = '/', [0x39] = ' ',
};

static const char keymap_shift[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
    [0x1A] = '{', [0x1B] = '}', [0x27] = ':', [0x28] = '"',
    [0x29] = '~', [0x2B] = '|', [0x33] = '<', [0x34] = '>',
    [0x35] = '?',
};

static volatile char buffer[KEYBOARD_BUFFER_SIZE];
static volatile uint16_t buffer_head;
static volatile uint16_t buffer_tail;
static int shift_down;
static int caps_lock;
static int extended;

static void buffer_push(char c)
{
    if ((uint16_t)(buffer_head - buffer_tail) >= KEYBOARD_BUFFER_SIZE)
        return; /* full: drop the newest character */
    buffer[buffer_head & (KEYBOARD_BUFFER_SIZE - 1)] = c;
    buffer_head++;
}

static char translate(uint8_t code)
{
    char c = keymap[code];

    if (c == '\0')
        return '\0';

    if (c >= 'a' && c <= 'z') {
        if (shift_down ^ caps_lock)
            c = (char)(c - 'a' + 'A');
        return c;
    }

    if (shift_down) {
        char shifted = keymap_shift[code];
        if (shifted != '\0')
            c = shifted;
    }
    return c;
}

static void keyboard_irq(void)
{
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    if (scancode == 0xE0) {
        extended = 1;
        return;
    }
    if (extended) {
        extended = 0; /* arrows, delete, ... are not used by the shell */
        return;
    }

    int released = scancode & 0x80;
    uint8_t code = scancode & 0x7F;

    if (code == 0x2A || code == 0x36) {
        shift_down = !released;
        return;
    }
    if (code == 0x3A) {
        if (!released)
            caps_lock = !caps_lock;
        return;
    }
    if (released)
        return;

    char c = translate(code);
    if (c != '\0')
        buffer_push(c);
}

void keyboard_init(void)
{
    /* Drain anything the firmware left in the controller. */
    while (inb(KEYBOARD_STATUS_PORT) & 0x01)
        (void)inb(KEYBOARD_DATA_PORT);

    irq_register_handler(1, keyboard_irq);
}

char keyboard_getchar(void)
{
    for (;;) {
        interrupts_disable();
        if (buffer_head != buffer_tail) {
            char c = buffer[buffer_tail & (KEYBOARD_BUFFER_SIZE - 1)];
            buffer_tail++;
            interrupts_enable();
            return c;
        }
        /* STI's shadow guarantees the CPU does not miss a wakeup here. */
        __asm__ volatile("sti; hlt" ::: "memory");
    }
}
