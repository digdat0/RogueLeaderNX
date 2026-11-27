// Minimal setjmp/longjmp for jumping back up the host stack of the current fiber.
//
// Saves every callee-saved general register of the ABI in use - Win64: rbx rbp rdi rsi rsp
// r12-r15 and xmm6-xmm15; System V x86-64 (Linux): rbx rbp rsp r12-r15 (no vector register is
// callee-saved there; rdi / rsi / xmm stay unused); AArch64 (Android): x19-x28, x29 (frame
// pointer), x30 (the return address: where it continues), sp and d8-d15. Unlike the C library's
// longjmp it does not unwind (no destructors run), which is what we want: the frames skipped are
// recompiled guest functions. threads.cpp sets up fresh fibers through it (the slots below).
#pragma once
#include <cstdint>

namespace rt {

struct alignas(16) JmpBuf {
    // x86-64: gpr = rbx rbp rdi rsi rsp r12 r13 r14 r15 rip, xmm = xmm6-xmm15.
    // AArch64: gpr = x19-x28, pad = x29 x30, xmm[0] = sp, xmm[1..4] = d8-d15.
    uint64_t gpr[10];
    uint64_t pad[2];
    uint8_t xmm[10][16];

#if defined(__aarch64__)
    // A fresh fiber (threads.cpp initJb): its stack, where it starts, and two values the
    // trampoline hands to the entry function (callee-saved registers survive the jump).
    uint64_t& sp() { return *reinterpret_cast<uint64_t*>(xmm[0]); }
    uint64_t sp() const { return *reinterpret_cast<const uint64_t*>(xmm[0]); }
    uint64_t& pc() { return pad[1]; }
    uint64_t& arg() { return gpr[0]; }    // x19
    uint64_t& entry() { return gpr[1]; }  // x20
#else
    uint64_t& sp() { return gpr[4]; }
    uint64_t sp() const { return gpr[4]; }
    uint64_t& pc() { return gpr[9]; }
    uint64_t& arg() { return gpr[0]; }    // rbx
    uint64_t& entry() { return gpr[5]; }  // r12
#endif
};

}  // namespace rt

extern "C" __attribute__((returns_twice)) int rt_setjmp(rt::JmpBuf* b);
extern "C" [[noreturn]] void rt_longjmp(rt::JmpBuf* b, int v);
