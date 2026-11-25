#if defined(__aarch64__)
.text
.align 2
.global x86_fiber_switch
.type x86_fiber_switch, %function
x86_fiber_switch:
    .cfi_startproc
    stp x19, x20, [x0, #0x00]
    stp x21, x22, [x0, #0x10]
    stp x23, x24, [x0, #0x20]
    stp x25, x26, [x0, #0x30]
    stp x27, x28, [x0, #0x40]
    stp x29, x30, [x0, #0x50]
    mov x9, sp
    str x9, [x0, #0x60]
    stp d8,  d9,  [x0, #0x70]
    stp d10, d11, [x0, #0x80]
    stp d12, d13, [x0, #0x90]
    stp d14, d15, [x0, #0xA0]

    ldp x19, x20, [x1, #0x00]
    ldp x21, x22, [x1, #0x10]
    ldp x23, x24, [x1, #0x20]
    ldp x25, x26, [x1, #0x30]
    ldp x27, x28, [x1, #0x40]
    ldp x29, x30, [x1, #0x50]
    ldr x9, [x1, #0x60]
    mov sp, x9
    ldp d8,  d9,  [x1, #0x70]
    ldp d10, d11, [x1, #0x80]
    ldp d12, d13, [x1, #0x90]
    ldp d14, d15, [x1, #0xA0]
    ret
    .cfi_endproc
.size x86_fiber_switch, .-x86_fiber_switch

.align 2
.global x86_fiber_trampoline
.type x86_fiber_trampoline, %function
x86_fiber_trampoline:
    .cfi_startproc
    .cfi_undefined x30
    .cfi_undefined x29
    mov x29, #0
    mov x30, #0
    mov x0, x20
    blr x19
    brk #0
    .cfi_endproc
.size x86_fiber_trampoline, .-x86_fiber_trampoline

.section .note.GNU-stack,"",%progbits
#endif
