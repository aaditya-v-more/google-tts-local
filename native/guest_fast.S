// AArch64 implementations of common Linux imports. This blob is copied into
// guest memory unchanged; it has no references outside the blob.
.text
.p2align 4
.globl _guest_fast_start, _guest_fast_end
.globl _guest_fast_memcpy, _guest_fast_memmove, _guest_fast_memset
.globl _guest_fast_strlen, _guest_fast_strnlen, _guest_fast_strcmp
.globl _guest_fast_memcmp, _guest_fast_memchr
.globl _native_tlsdesc
_guest_fast_start:
_guest_fast_memcpy:
    mov x3, x0
1:  cmp x2, #16
    b.lo 2f
    ldp x4, x5, [x1], #16
    stp x4, x5, [x3], #16
    sub x2, x2, #16
    b 1b
2:  cbz x2, 4f
3:  ldrb w4, [x1], #1
    strb w4, [x3], #1
    subs x2, x2, #1
    b.ne 3b
4:  ret

_guest_fast_memmove:
    cmp x0, x1
    b.lo _guest_fast_memcpy
    sub x3, x0, x1
    cmp x3, x2
    b.hs _guest_fast_memcpy
    add x3, x0, x2
    add x1, x1, x2
1:  cmp x2, #16
    b.lo 2f
    ldp x4, x5, [x1, #-16]!
    stp x4, x5, [x3, #-16]!
    sub x2, x2, #16
    b 1b
2:  cbz x2, 4f
3:  ldrb w4, [x1, #-1]!
    strb w4, [x3, #-1]!
    subs x2, x2, #1
    b.ne 3b
4:  ret

_guest_fast_memset:
    mov x3, x0
    and x1, x1, #0xff
    orr x1, x1, x1, lsl #8
    orr x1, x1, x1, lsl #16
    orr x1, x1, x1, lsl #32
1:  cmp x2, #16
    b.lo 2f
    stp x1, x1, [x3], #16
    sub x2, x2, #16
    b 1b
2:  cbz x2, 4f
3:  strb w1, [x3], #1
    subs x2, x2, #1
    b.ne 3b
4:  ret

_guest_fast_strlen:
    mov x1, x0
1:  ldrb w2, [x1], #1
    cbnz w2, 1b
    sub x0, x1, x0
    sub x0, x0, #1
    ret

_guest_fast_strnlen:
    mov x2, #0
1:  cmp x2, x1
    b.hs 2f
    ldrb w3, [x0, x2]
    cbz w3, 2f
    add x2, x2, #1
    b 1b
2:  mov x0, x2
    ret

_guest_fast_strcmp:
1:  ldrb w2, [x0], #1
    ldrb w3, [x1], #1
    cmp w2, w3
    b.ne 2f
    cbnz w2, 1b
2:  sub w0, w2, w3
    ret

_guest_fast_memcmp:
    cbz x2, 3f
1:  ldrb w3, [x0], #1
    ldrb w4, [x1], #1
    cmp w3, w4
    b.ne 2f
    subs x2, x2, #1
    b.ne 1b
2:  sub w0, w3, w4
    ret
3:  mov w0, #0
    ret

_guest_fast_memchr:
    and w1, w1, #0xff
    cbz x2, 3f
1:  ldrb w3, [x0]
    cmp w3, w1
    b.eq 2f
    add x0, x0, #1
    subs x2, x2, #1
    b.ne 1b
3:  mov x0, #0
2:  ret
_native_tlsdesc:
    ldr x0, [x0, #8]
    ret
_guest_fast_end:
