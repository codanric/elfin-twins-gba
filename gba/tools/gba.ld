/*
 * GBA cartridge linker script, used when building WITHOUT devkitARM.
 * Section conventions match devkitARM's gba_cart.ld:
 *   .iwram / *.iwram.o code  -> IWRAM (32-bit bus, 0 wait states)
 *   .ewram                   -> EWRAM
 *   .sbss                    -> EWRAM, zero-filled
 */
OUTPUT_FORMAT("elf32-littlearm", "elf32-bigarm", "elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY {
    rom   (rx)  : ORIGIN = 0x08000000, LENGTH = 32M
    iwram (rwx) : ORIGIN = 0x03000000, LENGTH = 32K
    ewram (rwx) : ORIGIN = 0x02000000, LENGTH = 256K
}

SECTIONS {
    .text : {
        KEEP(*(.crt0))
        *(EXCLUDE_FILE(*.iwram.o) .text EXCLUDE_FILE(*.iwram.o) .text.*)
        *(.gnu.linkonce.t.*)
        *(.glue_7) *(.glue_7t) *(.vfp11_veneer) *(.v4_bx)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
    } > rom

    .rodata : {
        *(.rodata .rodata.* .gnu.linkonce.r.*)
        . = ALIGN(4);
    } > rom

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > rom
    .ARM.exidx : {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
    } > rom

    .preinit_array : {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > rom
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > rom
    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > rom

    .iwram : {
        __iwram_start = .;
        *(.iwram .iwram.*)
        *.iwram.o(.text .text.*)
        . = ALIGN(4);
        __iwram_code_end = .;
    } > iwram AT > rom
    __iwram_lma = LOADADDR(.iwram);

    .data : {
        __data_start = .;
        *(.data .data.* .gnu.linkonce.d.*)
        . = ALIGN(4);
        __data_end = .;
    } > iwram AT > rom
    __data_lma = LOADADDR(.data);

    .bss (NOLOAD) : {
        __bss_start = .;
        __bss_start__ = .;
        *(.bss .bss.* .gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
        __bss_end__ = .;
    } > iwram
    __iwram_end = .;

    .ewram : {
        __ewram_start = .;
        *(.ewram .ewram.*)
        . = ALIGN(4);
        __ewram_end = .;
    } > ewram AT > rom
    __ewram_lma = LOADADDR(.ewram);

    .sbss (NOLOAD) : {
        __sbss_start = .;
        *(.sbss .sbss.*)
        . = ALIGN(4);
        __sbss_end = .;
    } > ewram

    /* heap for newlib's sbrk: rest of EWRAM */
    end = .;
    _end = .;
    __end__ = .;
    __heap_limit = 0x02040000;

    /DISCARD/ : { *(.note.GNU-stack) *(.comment) }
}

/* Leave room for the IRQ/user stacks at the top of IWRAM. */
ASSERT(__iwram_end <= 0x03007000, "IWRAM overflow: code+data+bss must leave 4 KB for the stacks")
