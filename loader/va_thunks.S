# SysV variadic entry points. clang refuses va_start in a sysv_abi function on Windows, so each thunk
# spills the six GP and eight XMM argument registers into a SysV register save area, builds the
# va_list struct { gp_offset, fp_offset, overflow_arg_area, reg_save_area } and calls the v-form
# with that va_list as the argument after the named ones (in register \reg).
.text
.macro VATHUNK name, impl, named, reg
.globl \name
\name:
    push %rbp
    mov %rsp, %rbp
    sub $208, %rsp
    mov %rdi, 0(%rsp)
    mov %rsi, 8(%rsp)
    mov %rdx, 16(%rsp)
    mov %rcx, 24(%rsp)
    mov %r8, 32(%rsp)
    mov %r9, 40(%rsp)
    movups %xmm0, 48(%rsp)
    movups %xmm1, 64(%rsp)
    movups %xmm2, 80(%rsp)
    movups %xmm3, 96(%rsp)
    movups %xmm4, 112(%rsp)
    movups %xmm5, 128(%rsp)
    movups %xmm6, 144(%rsp)
    movups %xmm7, 160(%rsp)
    movl $(\named * 8), 176(%rsp)
    movl $48, 180(%rsp)
    lea 16(%rbp), %rax
    mov %rax, 184(%rsp)
    mov %rsp, 192(%rsp)
    lea 176(%rsp), \reg
    call \impl
    leave
    ret
.endm

VATHUNK va_snprintf, sv_vsnprintf, 3, %rcx
VATHUNK va_sprintf, sv_vsprintf, 2, %rdx
VATHUNK va_printf, sv_vprintf, 1, %rsi
VATHUNK va_fprintf, sv_vfprintf, 2, %rdx
VATHUNK va_sscanf, sv_vsscanf, 2, %rdx
VATHUNK va_fscanf, sv_vfscanf, 2, %rdx
VATHUNK va_log_print, sv_log_vprint, 3, %rcx
VATHUNK va_syslog, sv_vsyslog, 2, %rdx
VATHUNK va_log_assert, sv_log_vassert, 3, %rcx
