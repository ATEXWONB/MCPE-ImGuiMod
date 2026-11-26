.text
.global vmhook_common
.type vmhook_common, %function
vmhook_common:
    sub     sp, sp, #256
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    str     x8, [sp, #64]
    stp     q0, q1, [sp, #80]
    stp     q2, q3, [sp, #112]
    stp     q4, q5, [sp, #144]
    stp     q6, q7, [sp, #176]
    stp     x9, x30, [sp, #208]
    mov     x0, x9
    mov     x1, x10
    add     x2, sp, #0
    bl      vmhook_log
    mov     x16, x0
    ldp     x9, x30, [sp, #208]
    ldp     q6, q7, [sp, #176]
    ldp     q4, q5, [sp, #144]
    ldp     q2, q3, [sp, #112]
    ldp     q0, q1, [sp, #80]
    ldr     x8, [sp, #64]
    ldp     x6, x7, [sp, #48]
    ldp     x4, x5, [sp, #32]
    ldp     x2, x3, [sp, #16]
    ldp     x0, x1, [sp, #0]
    add     sp, sp, #256
    br      x16

.global vmhook_ret0
.type vmhook_ret0, %function
vmhook_ret0:
    mov     x0, #0
    ret
