.text
.p2align 2
.weak_reference _os_set_custom_x18_abi_enabled

// A fixed bank of RX entry points avoids generating executable import stubs
// after ELF relocation. x16 is caller-saved in both AArch64 ABIs.
.globl _native_import_stubs
_native_import_stubs:
.set import_id, 1
.rept 2048
    mov w16, #import_id
    b _native_import_entry
    .set import_id, import_id + 1
.endr

.p2align 2
.globl _native_import_entry
_native_import_entry:
    sub sp, sp, #256
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    str x8, [sp, #64]
    str x18, [sp, #72]
    add x9, sp, #256
    str x9, [sp, #80]
    str x30, [sp, #88]
    mrs x9, fpcr
    str x9, [sp, #96]
    mrs x9, fpsr
    str x9, [sp, #104]
    str x16, [sp, #112]
    stp q0, q1, [sp, #128]
    stp q2, q3, [sp, #160]
    stp q4, q5, [sp, #192]
    stp q6, q7, [sp, #224]
    mov w0, #0
    bl _os_set_custom_x18_abi_enabled
    mov x0, sp
    bl _native_host_dispatch
    mov w0, #1
    bl _os_set_custom_x18_abi_enabled
    ldr x18, [sp, #72]
    ldr x9, [sp, #96]
    msr fpcr, x9
    ldr x9, [sp, #104]
    msr fpsr, x9
    ldp q0, q1, [sp, #128]
    ldp q2, q3, [sp, #160]
    ldp q4, q5, [sp, #192]
    ldp q6, q7, [sp, #224]
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldr x8, [sp, #64]
    ldr x30, [sp, #88]
    add sp, sp, #256
    ret

.globl _native_guest_enter
_native_guest_enter:
    stp x29, x30, [sp, #-80]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    mrs x21, fpcr
    mrs x22, fpsr
    mov x19, x0
    mov x20, x1
    mov w0, #1
    bl _os_set_custom_x18_abi_enabled
    mov x18, #0
    ldr x9, [x20, #192]
    msr fpcr, x9
    ldr x9, [x20, #200]
    msr fpsr, x9
    ldp q0, q1, [x20, #64]
    ldp q2, q3, [x20, #96]
    ldp q4, q5, [x20, #128]
    ldp q6, q7, [x20, #160]
    ldp x0, x1, [x20, #0]
    ldp x2, x3, [x20, #16]
    ldp x4, x5, [x20, #32]
    ldp x6, x7, [x20, #48]
    blr x19
    str x0, [x20, #208]
    stp q0, q1, [x20, #64]
    stp q2, q3, [x20, #96]
    stp q4, q5, [x20, #128]
    stp q6, q7, [x20, #160]
    mrs x9, fpcr
    str x9, [x20, #192]
    mrs x9, fpsr
    str x9, [x20, #200]
    mov w0, #0
    bl _os_set_custom_x18_abi_enabled
    msr fpcr, x21
    msr fpsr, x22
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x29, x30, [sp], #80
    ret

// Called only after checking the signed entitlement. On a failed check, do
// not write x18 or call the strict disable operation after the mode was lost.
.globl _native_switch_probe
_native_switch_probe:
    stp x29, x30, [sp, #-64]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    mov x19, x0
    mov x21, x1
    mov w0, #1
    bl _os_set_custom_x18_abi_enabled
    mrs x9, tpidr_el0
    tbz x9, #48, 3f
    movz x20, #0x4567
    movk x20, #0x0123, lsl #16
    movk x20, #0xcdef, lsl #32
    movk x20, #0x89ab, lsl #48
    mov x18, x20
    mov w9, #1
    stlr w9, [x21]
    mov x0, x19
    mov x16, #-36
    svc #0x80
    mov x22, x0
    mrs x9, tpidr_el0
    tbz x9, #48, 3f
    cbnz x22, 2f
    cmp x18, x20
    b.ne 2f
    mov x19, #0
    b 4f
2:
    mov x19, #1
4:
    mov w0, #0
    bl _os_set_custom_x18_abi_enabled
    b 5f
3:
    // Wake the competitor even if the mode disappeared before waiting.
    mov w9, #1
    stlr w9, [x21]
    mov x19, #1
5:
    mov x0, x19
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #64
    ret
