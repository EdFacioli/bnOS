#ifndef BNOS_IDT_H
#define BNOS_IDT_H

#include <stdint.h>

typedef void (*irq_handler_t)(void);

void idt_init(void);
void irq_register_handler(uint8_t irq, irq_handler_t handler);

#endif /* BNOS_IDT_H */
