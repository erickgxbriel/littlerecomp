#if defined(__x86_64__)
.text
.p2align 4
.global x86_fiber_switch
.type x86_fiber_switch, @function
x86_fiber_switch:
    .cfi_startproc
    movq %rbx, 0x00(%rdi)
    movq %rbp, 0x08(%rdi)
    movq %r12, 0x10(%rdi)
    movq %r13, 0x18(%rdi)
    movq %r14, 0x20(%rdi)
    movq %r15, 0x28(%rdi)
    leaq 8(%rsp), %rax
    movq %rax, 0x30(%rdi)
    movq (%rsp), %rax
    movq %rax, 0x38(%rdi)
    stmxcsr 0x40(%rdi)
    fnstcw 0x44(%rdi)

    ldmxcsr 0x40(%rsi)
    fldcw 0x44(%rsi)
    movq 0x00(%rsi), %rbx
    movq 0x08(%rsi), %rbp
    movq 0x10(%rsi), %r12
    movq 0x18(%rsi), %r13
    movq 0x20(%rsi), %r14
    movq 0x28(%rsi), %r15
    movq 0x30(%rsi), %rsp
    jmpq *0x38(%rsi)
    .cfi_endproc
.size x86_fiber_switch, .-x86_fiber_switch

.p2align 4
.global x86_fiber_trampoline
.type x86_fiber_trampoline, @function
x86_fiber_trampoline:
    .cfi_startproc
    .cfi_undefined rip
    .cfi_undefined rbp
    xorl %ebp, %ebp
    movq %r13, %rdi
    callq *%r12
    ud2
    .cfi_endproc
.size x86_fiber_trampoline, .-x86_fiber_trampoline

.section .note.GNU-stack,"",@progbits
#endif
