@ ---------------------------------------------------------------------------
@ Minimal GBA cartridge start-up code, used when building WITHOUT devkitARM
@ (devkitARM builds use its own gba_crt0.s via -specs=gba.specs).
@
@ - ROM header (logo, title, checksum are filled in by tools/gbafix.py)
@ - sets IRQ / user stacks
@ - copies .iwram, .data and .ewram from ROM, clears .bss and .sbss
@ - runs C/C++ constructors and calls main()
@ ---------------------------------------------------------------------------
    .section .crt0, "ax", %progbits
    .arm
    .align 2
    .global _start
    .type   _start, %function
_start:
    b       rom_start

    @ Nintendo logo (156 bytes) - written by gbafix
    .fill   156, 1, 0
    @ Title (12), game code (4), maker code (2)
    .fill   12, 1, 0
    .ascii  "0000"
    .ascii  "00"
    .byte   0x96            @ fixed value
    .byte   0x00            @ main unit code
    .byte   0x00            @ device type
    .fill   7, 1, 0         @ reserved
    .byte   0x00            @ software version
    .byte   0x00            @ header checksum (gbafix)
    .fill   2, 1, 0         @ reserved

rom_start:
    @ IRQ mode stack
    mov     r0, #0x12
    msr     cpsr_c, r0
    ldr     sp, =0x03007FA0
    @ System mode (user registers) stack
    mov     r0, #0x1F
    msr     cpsr_c, r0
    ldr     sp, =0x03007F00

    @ copy .iwram
    ldr     r0, =__iwram_lma
    ldr     r1, =__iwram_start
    ldr     r2, =__iwram_code_end
    bl      copy_words
    @ copy .data
    ldr     r0, =__data_lma
    ldr     r1, =__data_start
    ldr     r2, =__data_end
    bl      copy_words
    @ clear .bss
    ldr     r1, =__bss_start
    ldr     r2, =__bss_end
    bl      clear_words
    @ copy .ewram
    ldr     r0, =__ewram_lma
    ldr     r1, =__ewram_start
    ldr     r2, =__ewram_end
    bl      copy_words
    @ clear .sbss (EWRAM bss)
    ldr     r1, =__sbss_start
    ldr     r2, =__sbss_end
    bl      clear_words

    @ constructors
    ldr     r0, =__libc_init_array
    mov     lr, pc
    bx      r0

    @ main(0, 0)
    mov     r0, #0
    mov     r1, #0
    ldr     r3, =main
    mov     lr, pc
    bx      r3
1:  b       1b

@ r0 = src, r1 = dst, r2 = dst end
    .type   copy_words, %function
copy_words:
    cmp     r1, r2
    ldrlo   r3, [r0], #4
    strlo   r3, [r1], #4
    blo     copy_words
    bx      lr

@ r1 = dst, r2 = dst end
    .type   clear_words, %function
clear_words:
    mov     r3, #0
1:  cmp     r1, r2
    strlo   r3, [r1], #4
    blo     1b
    bx      lr

    .pool

@ newlib expects these when not using its own crt0
    .global _init
    .type   _init, %function
_init:
    bx      lr

    .global _fini
    .type   _fini, %function
_fini:
    bx      lr
