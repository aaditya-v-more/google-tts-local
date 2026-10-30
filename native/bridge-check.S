.text
.globl _bridge_guest
_bridge_guest:
    stp x29, x30, [sp, #-48]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    mov x19, x1
    mov x20, x0
    movz x18, #0x4567
    movk x18, #0x0123, lsl #16
    movk x18, #0xcdef, lsl #32
    movk x18, #0x89ab, lsl #48
    blr x20
    cmp x0, x19
    b.ne 1f
    movz x9, #0x4567
    movk x9, #0x0123, lsl #16
    movk x9, #0xcdef, lsl #32
    movk x9, #0x89ab, lsl #48
    cmp x18, x9
    cset x0, ne
    b 2f
1:
    mov x0, #2
2:
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #48
    ret

.globl _bridge_trap_guest
_bridge_trap_guest:
    brk #0
    ret

.globl _bridge_bad_access_guest
_bridge_bad_access_guest:
    mov x0, #0
    ldr x0, [x0]
    ret

.globl _bridge_alloc_guest
_bridge_alloc_guest:
    stp x29, x30, [sp, #-64]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    mov x19, x0
    mov x20, x1
    mov x21, x2
    movz x18, #0x4567
    movk x18, #0x123, lsl #16
    movk x18, #0xcdef, lsl #32
    movk x18, #0x89ab, lsl #48
1:
    and x0, x21, #0x1ff
    add x0, x0, #16
    blr x19
    cbz x0, 3f
    str x18, [x0]
    ldr x22, [x0]
    cmp x22, x18
    b.ne 3f
    blr x20
    subs x21, x21, #1
    b.ne 1b
    mov x0, #0
    b 4f
3:
    mov x0, #1
4:
    ldp x23, x24, [sp, #48]
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #64
    ret
