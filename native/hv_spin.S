.text
.p2align 2
.globl _guest_spin_start
.globl _guest_spin_end
// In the guest: x0 = iteration count, x1 = expected x18. Return x2 = changes.
_guest_spin_start:
    mov x2, #0
1:
    cmp x18, x1
    b.eq 2f
    add x2, x2, #1
    mov x18, x1
2:
    subs x0, x0, #1
    b.ne 1b
    .long 0xd4000002 // hvc #0
_guest_spin_end:
