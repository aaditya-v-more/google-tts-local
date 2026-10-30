#ifndef GTTS_GUEST_NATIVE_H
#define GTTS_GUEST_NATIVE_H

#include <stdint.h>
#include <stddef.h>

/* This is also the layout consumed by guest_native.S. */
typedef struct {
    uint64_t x[9], x18, sp, lr, fpcr, fpsr, id, reserved;
    __uint128_t q[8];
} NativeFrame;

typedef struct {
    uint64_t args[8];
    __uint128_t q[8];
    uint64_t fpcr, fpsr, result;
} NativeCall;

_Static_assert(offsetof(NativeFrame, q) == 128 && sizeof(NativeFrame) == 256, "native exit frame layout");
_Static_assert(offsetof(NativeCall, q) == 64 && offsetof(NativeCall, fpcr) == 192, "native entry frame layout");

extern unsigned guest_native_tls_key;
extern unsigned guest_native_current_key;
void native_guest_enter(uint64_t function, NativeCall *call);
int native_switch_probe(unsigned semaphore, int *waiting);
void native_host_dispatch(NativeFrame *frame);

#endif
