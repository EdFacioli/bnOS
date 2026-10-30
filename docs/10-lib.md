# 10. Freestanding runtime helpers

Files: `src/lib.c`, `src/lib.h`.

`lib.c` provides the small subset of C library functionality the kernel needs:
three memory functions and four string/number functions. It exists because the
kernel is compiled with `-ffreestanding -fno-builtin` and linked with
`-nostdlib`: there is no libc to call, but the compiler may still emit calls to
`memset`, `memcpy` or `memmove` when lowering struct copies, aggregate
initialisation or large assignments.

## 10.1 Why these functions, specifically?

| Function | Used by | Why |
|---|---|---|
| `memset` | `pmm_init` | fill both 128 KiB bitmaps |
| `memcpy` | compiler-generated | struct/array copies |
| `memmove` | compiler-generated | overlapping copies |
| `strlen` | `pmm_init` | reserve the Multiboot command line |
| `strcmp` | `shell.c` | command lookup |
| `strncmp` | (available) | prefix comparisons, not currently used |
| `strtoul` | `shell.c` | parse the `free <addr>` argument |

The compiler is allowed to turn a hand-written byte loop into a call to
`memcpy`/`memset` even in freestanding mode. Compiling with `-fno-builtin`
prevents GCC from recognising the implementations themselves and turning them
into recursive calls. Without that flag, a naive `memcpy` implementation can
compile into a call to `memcpy`, which then calls itself forever.

The kernel does **not** provide `malloc`, `free`, `printf`, `memcmp`, `strcpy`,
`strcat`, `strchr`, `atoi`, `qsort` or anything else. Anything that needs them
must implement them locally or use the VGA/shell equivalents.

## 10.2 `memset`

```c
void *memset(void *dst, int value, size_t n)
{
    uint8_t *d = dst;
    while (n--)
        *d++ = (uint8_t)value;
    return dst;
}
```

* Writes `n` bytes of `(uint8_t)value` starting at `dst`.
* Returns `dst`, matching the standard signature.
* The cast truncates `value` to one byte; `memset(p, 0x1234, n)` writes `0x34`.
* `n == 0` performs no writes. The `while (n--)` form is safe because the
  post-decrement is evaluated before the loop body; when `n` is 0 the condition
  is false and `n` wraps to `SIZE_MAX`, but the loop exits immediately and `n`
  is not used again.
* No alignment assumptions, no word-at-a-time optimisation. The PMM calls it
  with 128 KiB, so this is 131,072 byte stores; at boot time that is
  negligible.

## 10.3 `memcpy`

```c
void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}
```

* Copies `n` bytes forward.
* Returns `dst`.
* **Undefined for overlapping ranges.** If `dst` and `src` overlap,
  `memmove` must be used. The compiler only emits `memcpy` for non-overlapping
  copies; C code must choose correctly.
* `n == 0` is a no-op.
* No alignment or bulk optimisation.

## 10.4 `memmove`

```c
void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}
```

Overlap handling:

* If `dst < src`, copy **forward** (low to high). The destination is behind the
  source, so writing forward cannot overwrite source bytes that have not been
  read yet.
* If `dst > src`, copy **backward** (high to low). The destination is ahead of
  the source, so writing backward cannot overwrite unread source bytes.
* If `dst == src`, do nothing.

Example: `memmove(buf+1, buf, 4)` with `dst > src` copies from the end:
`buf[4] = buf[3]`, `buf[3] = buf[2]`, etc., preserving the source.

`d += n` / `s += n` point one past the end, and `*--d` pre-decrements before
the store, so the first write is at `dst + n - 1`. When `n == 0`, the loops do
not execute (the `while (n--)` condition is false).

This function is used by the compiler for overlapping aggregate copies. The
kernel itself does not call it directly.

## 10.5 `strlen`

```c
size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len] != '\0')
        len++;
    return len;
}
```

Counts bytes until the first NUL, not including it. No NULL check; passing NULL
is undefined and will fault (or, because address 0 is mapped in this kernel,
read low memory until a zero byte). The PMM calls it on the Multiboot command
line, which the bootloader guarantees is NUL-terminated.

## 10.6 `strcmp`

```c
int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}
```

Compares byte by byte until a difference or the end of `a`.

* Bytes are treated as **unsigned** (`uint8_t`) before subtraction. The C
  standard only guarantees a negative/zero/positive result, but using unsigned
  bytes gives the conventional ordering for ASCII and avoids sign-extension
  surprises.
* If `a` is a prefix of `b`, the loop stops when `*a == '\0'`; the result is
  `0 - *b`, which is negative.
* If `b` is a prefix of `a`, the loop stops when `*a != *b` (because `*b` is
  NUL); the result is positive.
* Equal strings return 0.
* The function is case-sensitive and locale-independent.

`shell.c` uses it for exact command lookup.

## 10.7 `strncmp`

```c
int strncmp(const char *a, const char *b, size_t n)
{
    while (n > 0 && *a != '\0' && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0)
        return 0;
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}
```

Compares at most `n` bytes.

* If the first `n` bytes are equal, `n` reaches 0 and the result is 0. This is
  true even if both strings continue beyond `n`.
* If a difference or the end of `a` is found first, the difference of the
  current bytes is returned.
* `n == 0` returns 0 without dereferencing either pointer.
* As with `strcmp`, bytes are unsigned.

Not used in the current shell, but useful for future prefix matching
(`help`/`halt`, etc.).

## 10.8 `strtoul`

```c
unsigned long strtoul(const char *s, char **end, int base)
{
    const char *p = s;
    unsigned long value = 0;
    int negative = 0;
    int digit;

    while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
        p++;

    if (*p == '+') {
        p++;
    } else if (*p == '-') {
        negative = 1;
        p++;
    }

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = (*p == '0') ? 8 : 10;
    }

    if (base < 2 || base > 36) {
        if (end)
            *end = (char *)s;
        return 0;
    }

    while ((digit = digit_value(*p)) >= 0 && digit < base) {
        value = value * (unsigned long)base + (unsigned long)digit;
        p++;
    }

    if (end)
        *end = (char *)p;
    return negative ? 0UL - value : value;
}
```

This is a compact `strtoul`, not a complete C-standard implementation. Walk
through it:

### Whitespace

Leading spaces and ASCII whitespace `'\t'` (0x09) through `'\r'` (0x0D) are
skipped. This includes `\n`, `\v` and `\f`, because they fall between `\t` and
`\r`. Other Unicode whitespace is not recognised.

### Sign

An optional `+` or `-` is consumed. A negative sign sets a flag but does not
stop parsing. The final result is negated in unsigned arithmetic:
`0UL - value`, i.e. two's complement. On i386, `unsigned long` is 32 bits, so
`-1` becomes `0xFFFFFFFF`, and `-0x10` becomes `0xFFFFFFF0`.

### Base detection

* If `base` is 0 or 16 and the input starts with `0x`/`0X`, skip the prefix and
  use base 16.
* Otherwise, if `base` is 0, a leading `0` selects octal; anything else selects
  decimal.
* An explicit base 2..36 is used as-is. No `0b` binary prefix is recognised.
* If the resulting base is outside 2..36, the function sets `*end` to the
  **original** start pointer and returns 0. (The standard would consume
  nothing; this matches that.)

Note the prefix detection happens even for an explicit base 16, so
`strtoul("0x10", &end, 16)` returns 16 and consumes all four characters.

### Digit loop

```c
static int digit_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return -1;
}
```

Digits are accepted for bases up to 36 (`0-9`, `a-z`, `A-Z`). A character is a
digit only if `digit_value(c) < base`; for example, `'8'` is a valid digit for
base 10 but not for base 8, and `'g'` is valid for base 17+ but not base 16.

Conversion stops at the first invalid character. The loop does not check for
overflow: `value * base + digit` wraps modulo 2^32 (for 32-bit `unsigned long`)
instead of setting `ERANGE`. There is no `errno` and no `ULONG_MAX` clamp.

### End pointer

`*end` is set to the first unparsed character, after the sign, prefix and valid
digits. If no digits were consumed, `*end` points just after the sign/prefix (or
at the original string if no prefix was consumed). The shell uses this to reject
trailing garbage:

```c
address = (uint32_t)strtoul(argv[1], &end, 0);
if (end == argv[1] || (end != 0 && *end != '\0')) {
    vga_printf("invalid address: %s\n", argv[1]);
    return;
}
```

### Return value

The parsed magnitude is returned, negated if a `-` was present. The caller casts
to `uint32_t` on i386. Examples:

| Input | Base | Result | End points at |
|---|---|---|---|
| `"0x14C000"` | 0 | `0x0014C000` | end of string |
| `"1363968"` | 0 | `1363968` | end of string |
| `"0520000"` | 0 | octal value | end of string |
| `"010"` | 0 | 8 | end of string |
| `"08"` | 0 | 0 | `'8'` (octal rejects 8) |
| `"-1"` | 0 | `0xFFFFFFFF` | end of string |
| `"  +42"` | 0 | 42 | end of string |
| `"0x"` | 0 | 0 | after `0x` (no digits) |
| `"abc"` | 10 | 0 | at `a` |
| `"z"` | 36 | 35 | end of string |
| `"z"` | 16 | 0 | at `z` |

### Why not use a full implementation?

The shell only needs to parse a page address. A full `strtoul` would add
overflow detection, locale handling and more code for no benefit. The compact
version is easy to audit and sufficient for the command's validation logic.

## 10.9 Interaction with `-ffreestanding -fno-builtin`

* `-ffreestanding` tells GCC that the program may not have a hosted C library;
  it still knows the semantics of `memset`/`memcpy` and may emit calls to them.
* `-fno-builtin` prevents GCC from assuming the built-in semantics and
  transforming loops into calls to the very functions being defined. Without
  it, `memset`'s byte loop could be recognised and replaced with a call to
  `memset`.
* The functions use the standard names and signatures so the compiler's
  generated calls link against them.
* There are no `static` or `inline` qualifiers: the symbols must be globally
  visible at link time.
* The compiler is free to inline calls it can see, but the out-of-line
  definitions still exist for calls emitted in other translation units.

## 10.10 Sharp edges

* **No NULL checks.** `strlen(NULL)`, `strcmp(NULL, x)`, `strtoul(NULL, ...)`
  crash (or, with identity-mapped page 0, misbehave). Callers must pass valid
  pointers.
* **No bounds checking.** `memset`/`memcpy`/`memmove` trust `n`. A wrong length
  corrupts memory silently.
* **`memcpy` overlap is undefined.** Use `memmove` for overlapping copies.
* **No overflow detection in `strtoul`.** Large values wrap modulo 2^32.
* **`strtoul` accepts a negative sign.** `free -1` parses to `0xFFFFFFFF`,
  which the PMM rejects as out of range.
* **Octal surprise.** `010` is 8, not 10. This is standard `strtoul(base=0)`
  behaviour but can confuse users.
* **No `errno`.** Errors are reported only through the end pointer and a zero
  return.
* **Byte-at-a-time performance.** Fine for boot and shell use; not suitable for
  bulk I/O.
* **No `memcmp`.** The shell does not need it; future code would have to add
  it.
* **No `strncpy`, `strlcpy`, `strcat`.** Buffer assembly is done manually with
  `vga_write`/`vga_putc`.

Next: [11-end-to-end.md](11-end-to-end.md).
