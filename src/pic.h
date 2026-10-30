#ifndef BNOS_PIC_H
#define BNOS_PIC_H

#include <stdint.h>

void pic_remap(void);
void pic_unmask(uint8_t irq);
void pic_send_eoi(uint8_t irq);

#endif /* BNOS_PIC_H */
