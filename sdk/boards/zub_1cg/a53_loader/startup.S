.section .text
.globl _start
_start:
    b entry_point
    .balign 0x80
    b .
    .balign 0x80
    b .
    .balign 0x80
    b .
    .balign 0x80
    b .
    .balign 0x80
    b entry_point
    .balign 0x80
    b .
    .balign 0x80
    b .
    .balign 0x80
    b .

entry_point:
    mov x0, #0xFFFFFFFC
    mov sp, x0
    bl main
    b .