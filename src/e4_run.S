#if defined(__XTENSA__) && defined(__XTENSA_WINDOWED_ABI__)
    .text
    .align  4
    .global e4_run
    .type   e4_run, @function
# void e4_run(IU *ip, DU *sp, DU tos, DU *rp, VM &vm, Ret *out)
#              a2      a3     a4      a5      a6        a7
e4_run:
    entry   a1, 32
    mov     a10, a2             # ip
    mov     a11, a3             # sp
    mov     a12, a4             # tos
    mov     a13, a5             # rp
    beqz    a10, 2f             # ip == 0: nothing to run
1:  l32i    a8,  a10, 0         # fp = *ip
    mov     a14, a6             # vm
    addi    a10, a10, 4         # ip++
    callx8  a8                  # Ret comes back in a10..a13
    bnez    a10, 1b
2:  s32i    a10, a7, 0          # out->ip   (always 0 here)
    s32i    a11, a7, 4          # out->sp
    s32i    a12, a7, 8          # out->tos
    s32i    a13, a7, 12         # out->rp
    retw
    .size   e4_run, . - e4_run
#endif
