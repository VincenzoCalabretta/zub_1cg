OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(reset_handler)

MEMORY {
    OCM (rwx) : ORIGIN = 0xFFFF0000, LENGTH = 64K
}

_irq_stack_size = 0x0400;
_fiq_stack_size = 0x0200;
_abt_stack_size = 0x0200;
_und_stack_size = 0x0200;
_svc_stack_size = 0x1000;
_sys_stack_size = 0x0400;

SECTIONS {
    .vectors : {
        KEEP(*(.vectors))
    } >OCM

    .text : {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } >OCM

    .data : {
        _data_start = .;
        *(.data*)
        . = ALIGN(4);
        _data_end = .;
    } >OCM

    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } >OCM

    _tx_first_unused_memory = .;
    _stack_top = 0xFFFFFFFC;
}