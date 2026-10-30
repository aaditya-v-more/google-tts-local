.text
.p2align 2
.globl _test_x18
// Count x18 changes across ordinary Darwin calls while signals interrupt this thread.
// x0: iterations, x1: four counts followed by four first observed values.
_test_x18:
    stp x29, x30, [sp, #-80]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    str x18, [sp, #64]
    mov x19, x0
    mov x20, x1
    movz x21, #0x4567
    movk x21, #0x0123, lsl #16
    movk x21, #0xcdef, lsl #32
    movk x21, #0x89ab, lsl #48
    mov x18, x21
1:
    bl _sched_yield
    cmp x18, x21
    b.eq 2f
    ldr x22, [x20]
    cbnz x22, 10f
    str x18, [x20, #32]
10:
    add x22, x22, #1
    str x22, [x20]
    mov x18, x21
2:
    bl _getpid
    cmp x18, x21
    b.eq 3f
    ldr x22, [x20, #8]
    cbnz x22, 11f
    str x18, [x20, #40]
11:
    add x22, x22, #1
    str x22, [x20, #8]
    mov x18, x21
3:
    tst x19, #63
    b.ne 6f
    mov x0, #80
    bl _malloc
    cmp x18, x21
    b.eq 4f
    ldr x22, [x20, #16]
    cbnz x22, 12f
    str x18, [x20, #48]
12:
    add x22, x22, #1
    str x22, [x20, #16]
    mov x18, x21
4:
    bl _free
    cmp x18, x21
    b.eq 6f
    ldr x22, [x20, #24]
    cbnz x22, 13f
    str x18, [x20, #56]
13:
    add x22, x22, #1
    str x22, [x20, #24]
    mov x18, x21
6:
    subs x19, x19, #1
    b.ne 1b
    ldr x18, [sp, #64]
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x29, x30, [sp], #80
    ret

.globl _spin_x18
// No calls or syscalls: count changes during ordinary execution/interruptions.
// x0: iterations, x1: count and first observed value.
_spin_x18:
    str x18, [sp, #-16]!
    movz x2, #0x4567
    movk x2, #0x0123, lsl #16
    movk x2, #0xcdef, lsl #32
    movk x2, #0x89ab, lsl #48
    mov x18, x2
    mov x3, #0
1:
    cmp x18, x2
    b.eq 2f
    cbnz x3, 3f
    str x18, [x1, #8]
3:
    add x3, x3, #1
    mov x18, x2
2:
    subs x0, x0, #1
    b.ne 1b
    str x3, [x1]
    ldr x18, [sp], #16
    ret

.globl _spin_x18_pattern
// Only called while the custom x18 ABI is enabled.
// x0: iterations, x1: mismatch count, first value, lost-mode flag, remaining
// iterations; x2: per-thread pattern. Stop as soon as the mode is lost or
// x18 changes, and do not write x18 after observing a lost mode.
_spin_x18_pattern:
    mrs x4, TPIDR_EL0
    tbz x4, #48, 4f
    mov x18, x2
1:
    mrs x4, TPIDR_EL0
    tbz x4, #48, 4f
    cmp x18, x2
    b.eq 2f
    str x18, [x1, #8]
    mov x3, #1
    str x3, [x1]
    str x0, [x1, #24]
    ret
2:
    subs x0, x0, #1
    b.ne 1b
    ret
4:
    mov x3, #1
    str x3, [x1, #16]
    str x0, [x1, #24]
    ret

.globl _custom_x18_yields
// Entered in custom mode. Save the custom x18 value, leave the mode before
// calling sched_yield, then re-enter and restore it on the custom side only.
// x0: iterations, x1: mismatch count, first value, lost-mode flag, remaining
// iterations; x2: pattern.
_custom_x18_yields:
    stp x29, x30, [sp, #-80]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    mov x19, x0
    mov x20, x1
    mov x21, x2
    mrs x9, TPIDR_EL0
    tbz x9, #48, 4f
    mov x18, x21
1:
    mrs x9, TPIDR_EL0
    tbz x9, #48, 4f
    cmp x18, x21
    b.ne 3f
    str x18, [sp, #48]
    mov w0, #0
    bl _os_set_custom_x18_abi_enabled
    bl _sched_yield
    mov w0, #1
    bl _os_set_custom_x18_abi_enabled
    mrs x9, TPIDR_EL0
    tbz x9, #48, 4f
    ldr x18, [sp, #48]
    cmp x18, x21
    b.eq 2f
3:
    str x18, [x20, #8]
    mov x22, #1
    str x22, [x20]
    str x19, [x20, #24]
    b 5f
2:
    subs x19, x19, #1
    b.ne 1b
    b 5f
4:
    mov x22, #1
    str x22, [x20, #16]
    str x19, [x20, #24]
5:
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x29, x30, [sp], #80
    ret

.globl _custom_x18_mach_yields
// Directly enter the kernel's swtch trap without calling a Darwin library.
// The swtch stub on this system uses x16=-60, svc #0x80. This is a probe,
// not a production dependency on the private trap number.
// x0: iterations, x1: mismatch count, first value, lost-mode flag, remaining
// iterations; x2: pattern.
_custom_x18_mach_yields:
    stp x19, x20, [sp, #-48]!
    stp x21, x22, [sp, #16]
    mov x19, x0
    mov x20, x1
    mov x21, x2
    mrs x9, TPIDR_EL0
    tbz x9, #48, 4f
    mov x18, x21
1:
    mov x16, #-60
    svc #0x80
    mrs x9, TPIDR_EL0
    tbz x9, #48, 4f
    cmp x18, x21
    b.eq 2f
    str x18, [x20, #8]
    mov x22, #1
    str x22, [x20]
    str x19, [x20, #24]
    b 5f
2:
    subs x19, x19, #1
    b.ne 1b
    b 5f
4:
    mov x22, #1
    str x22, [x20, #16]
    str x19, [x20, #24]
5:
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp], #48
    ret
