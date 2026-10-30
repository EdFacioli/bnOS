#ifndef BNOS_PMM_H
#define BNOS_PMM_H

#include "multiboot.h"

#include <stdint.h>

/* Identity-mapped physical pages: no paging is enabled yet. */
void pmm_init(const struct multiboot_info *mbi, uint32_t kernel_end);
void *pmm_alloc_page(void);        /* caller may free it later */
void *pmm_alloc_kernel_page(void); /* permanently reserved (page tables) */
int pmm_free_page(void *page);

uint32_t pmm_total_pages(void);
uint32_t pmm_free_pages(void);

#endif /* BNOS_PMM_H */
