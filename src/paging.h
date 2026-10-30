#ifndef BNOS_PAGING_H
#define BNOS_PAGING_H

#include <stdint.h>

void paging_init(uint32_t kernel_end);
int paging_enabled(void);
uint32_t paging_directory_address(void);
uint32_t paging_mapped_pages(void);

/* Map/unmap one 4 KiB page. Addresses must be page aligned. */
int paging_map_page(uint32_t virt, uint32_t phys, int writable);
int paging_unmap_page(uint32_t virt);
int paging_is_mapped(uint32_t virt);

#endif /* BNOS_PAGING_H */
