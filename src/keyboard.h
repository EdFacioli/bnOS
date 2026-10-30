#ifndef BNOS_KEYBOARD_H
#define BNOS_KEYBOARD_H

/* Blocking: halts the CPU until a character is available. */
char keyboard_getchar(void);

void keyboard_init(void);

#endif /* BNOS_KEYBOARD_H */
