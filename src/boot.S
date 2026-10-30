/*
 * Multiboot v1 entry point.
 *
 * The bootloader (GRUB, or `qemu -kernel`) enters here in 32-bit protected
 * mode with interrupts disabled, paging off and a flat address space.
 * %eax = MULTIBOOT_BOOTLOADER_MAGIC, %ebx = physical address of the
 * multiboot info structure.
 */

.set MULTIBOOT_HEADER_MAGIC, 0x1BADB002
.set MULTIBOOT_HEADER_FLAGS, 0x00000003   /* page-align modules, memory info */

.section .multiboot, "a"
.align 4
    .long MULTIBOOT_HEADER_MAGIC
    .long MULTIBOOT_HEADER_FLAGS
    .long -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)

.section .bss
.align 16
stack_bottom:
    .skip 16384
stack_top:

.section .text
.global _start
.type _start, @function
_start:
    cli
    mov $stack_top, %esp

    /* Stash boot arguments in registers: the .bss zeroing below clobbers
       %eax/%ecx/%edi, and the stack itself lives inside .bss. */
    mov %eax, %edx
    mov %ebx, %esi

    /* Zero .bss: multiboot does not guarantee it is zeroed. */
    mov $__bss_start, %edi
    mov $__bss_end, %ecx
    sub %edi, %ecx
    xor %eax, %eax
    rep stosb

    /* kernel_main(magic, info) in cdecl order. */
    push %esi
    push %edx
    call kernel_main

    /* kernel_main must not return; halt forever if it does. */
1:  cli
    hlt
    jmp 1b

.size _start, . - _start
.section .note.GNU-stack, "", @progbits
