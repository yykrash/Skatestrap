# setjmp/longjmp for SysV callers. jmp_buf (bionic: 88 bytes): rbx rbp r12 r13 r14 r15 rsp rip, mxcsr, x87 cw.
.text
.globl sv_setjmp
sv_setjmp:
    mov %rbx, 0(%rdi)
    mov %rbp, 8(%rdi)
    mov %r12, 16(%rdi)
    mov %r13, 24(%rdi)
    mov %r14, 32(%rdi)
    mov %r15, 40(%rdi)
    lea 8(%rsp), %rax
    mov %rax, 48(%rdi)
    mov (%rsp), %rax
    mov %rax, 56(%rdi)
    stmxcsr 64(%rdi)
    fnstcw 68(%rdi)
    xor %eax, %eax
    ret

.globl sv_longjmp
sv_longjmp:
    mov %esi, %eax
    test %eax, %eax
    jnz 1f
    inc %eax
1:  mov 0(%rdi), %rbx
    mov 8(%rdi), %rbp
    mov 16(%rdi), %r12
    mov 24(%rdi), %r13
    mov 32(%rdi), %r14
    mov 40(%rdi), %r15
    ldmxcsr 64(%rdi)
    fldcw 68(%rdi)
    mov 48(%rdi), %rsp
    jmp *56(%rdi)
