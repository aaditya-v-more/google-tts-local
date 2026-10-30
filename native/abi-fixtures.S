.text
.p2align 2
.globl _abi_thread_start
.globl _abi_thread_end
_abi_thread_start:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
1:  ldar x9, [x0]
    cbz x9, 1b
    ldr x1, [x0, #8]
    mov x0, #0
    blr x9
    mov x0, #42
    ldp x29, x30, [sp], #16
    ret
_abi_thread_end:

.globl _abi_return_start
.globl _abi_return_end
_abi_return_start:
    mov x0, #42
    ret
_abi_return_end:

.globl _abi_callback_start
.globl _abi_callback_end
_abi_callback_start:
    ldr x3, [x0]
    str x3, [x2]
    mov x0, #1
    ret
_abi_callback_end:

.globl _abi_compare_start
.globl _abi_compare_end
_abi_compare_start:
    ldr w2, [x0]
    ldr w3, [x1]
    cmp w2, w3
    cset w2, gt
    cset w3, lt
    sub w0, w2, w3
    ret
_abi_compare_end:

.globl _abi_destructor_start
.globl _abi_destructor_end
_abi_destructor_start:
    mov w1, #42
    str w1, [x0]
    ret
_abi_destructor_end:

.globl _abi_destructor_order_start
.globl _abi_destructor_order_end
_abi_destructor_order_start:
    ldr x1, [x0]
    ldr w2, [x0, #8]
    ldr w3, [x1]
    lsl w3, w3, #4
    orr w3, w3, w2
    str w3, [x1]
    ret
_abi_destructor_order_end:
