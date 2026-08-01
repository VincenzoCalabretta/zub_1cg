OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

MEMORY {
    OCM (rwx) : ORIGIN = 0xFFFC0000, LENGTH = 0x40000
}

SECTIONS {
    .text 0xFFFC0000 : {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } >OCM

    .data : {
        *(.data*)
        . = ALIGN(4);
    } >OCM

    .bss (NOLOAD) : {
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
    } >OCM

    .stack (NOLOAD) : {
        . = . + 0x4000;
        _stack_top = .;
    } >OCM
}