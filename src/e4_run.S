#if defined(__XTENSA__) && defined(__XTENSA_WINDOWED_ABI__)
    .text
    .align  4
    .global e4_run
    .type   e4_run, @function
#if !defined(E4_RTOP) || E4_RTOP
# void e4_run(IU *ip, DU *sp, DU tos, DU *rp, VM &vm, Ret *out)
#              a2      a3     a4      a5      a6        a7
e4_run:
    entry   a1, 32
    mov     a10, a2             # ip
    mov     a11, a3             # sp
    mov     a12, a4             # tos
    mov     a13, a5             # rp
    beqz    a10, 2f
1:  l32i    a8,  a10, 0
    mov     a14, a6             # vm
    addi    a10, a10, 4
    callx8  a8                  # Ret in a10..a13
    bnez    a10, 1b
2:  s32i    a10, a7, 0
    s32i    a11, a7, 4
    s32i    a12, a7, 8
    s32i    a13, a7, 12
    retw
#else // !defined(E4_RTOP) || E4_RTOP
# void e4_run(IU *ip, DU *sp, DU tos, VM &vm, Ret *out)
#              a2      a3     a4      a5        a6
e4_run:
    entry   a1, 32
    mov     a10, a2             # ip
    mov     a11, a3             # sp
    mov     a12, a4             # tos
    beqz    a10, 2f
1:  l32i    a8,  a10, 0
    mov     a13, a5             # vm (4th arg), reload every time
    addi    a10, a10, 4
    callx8  a8                  # Ret in a10..a12
    bnez    a10, 1b
2:  s32i    a10, a6, 0          # out->ip
    s32i    a11, a6, 4          # out->sp
    s32i    a12, a6, 8          # out->tos
    retw
#endif // !defined(E4_RTOP) || E4_RTOP
    .size   e4_run, . - e4_run
#endif // defined(__XTENSA__) && defined(__XTENSA_WINDOWED_ABI__)
