OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(reset_handler)

/*
 * RAM is m3_mem_ctrl's BRAM as seen from the M3's own AXI master (mapped to
 * 0x0 in applications/orbtrace/vivado/create_bd.tcl, matching where an
 * ARMv7-M core always fetches its reset SP/PC). Its size must match that
 * BD's blk_mem_gen configuration (64K / 16384 x 32-bit).
 */
MEMORY {
    RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 64K
}

_stack_size = 0x0400;

PHDRS {
    text PT_LOAD FLAGS(5);  /* PF_R | PF_X */
    data PT_LOAD FLAGS(6);  /* PF_R | PF_W */
}

SECTIONS {
    .vectors : {
        KEEP(*(.vectors))
    } >RAM :text

    .text : {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } >RAM :text

    .data : {
        _data_start = .;
        *(.data*)
        . = ALIGN(4);
        _data_end = .;
    } >RAM :data

    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } >RAM :data

    _stack_top = ORIGIN(RAM) + LENGTH(RAM);
    _stack_bottom = _stack_top - _stack_size;

    ASSERT(_bss_end <= _stack_bottom, "M3 image overlaps the stack")
    ASSERT((ADDR(.vectors) & 0x7f) == 0,
           "M3 vector table (VTOR) must be 128-byte aligned")
    ASSERT(_stack_top <= ORIGIN(RAM) + LENGTH(RAM),
           "M3 stack exceeds RAM")
}
