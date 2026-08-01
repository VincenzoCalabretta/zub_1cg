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

/* Keep executable code and writable state in separate load segments. */
PHDRS {
    text PT_LOAD FLAGS(5);  /* PF_R | PF_X */
    data PT_LOAD FLAGS(6);  /* PF_R | PF_W */
}

SECTIONS {
    .vectors : {
        KEEP(*(.vectors))
    } >OCM :text

    .text : {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } >OCM :text

    .data : {
        _data_start = .;
        *(.data*)
        . = ALIGN(4);
        _data_end = .;
    } >OCM :data

    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } >OCM :data

    _tx_first_unused_memory = .;
    _stack_top = 0xFFFFFFFC;
    _stack_bottom = _stack_top - _irq_stack_size - _fiq_stack_size -
                    _abt_stack_size - _und_stack_size - _svc_stack_size -
                    _sys_stack_size;

    ASSERT(_tx_first_unused_memory <= _stack_bottom,
           "R5 image overlaps the exception-mode stacks")
}
