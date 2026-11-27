// RL-NX (Stage 11): FASTMEM's fault path (-DRL_NX_FASTMEM builds only; switch/CMakeLists.txt adds this file then).
//
// Horizon delivers a user-mode exception by running the process entry with x0 = the exception type and x1 = an
// exception info block (x0-x8, lr, sp, pc, pstate, afsr0/1, esr, far); svcReturnFromException(0) resumes the thread
// with x0-x8, lr, sp, pc and pstate taken back from that block (libnx's own handler resumes itself into its C
// handler that way). x9-x29 and the FP/SIMD registers are still the thread's own at entry. libnx's crt0 branches
// to __libnx_exception_entry, a weak symbol: this file replaces it.
//
// A data abort whose address is inside the fastmem window (g_rlFmBase, fastmem_switch.cpp) is a recompiled F
// access to memory that is not plain RAM. Nothing is done in the exception itself: the faulting thread's full
// context is written to a record below its stack pointer and the thread resumes in rl_fm_tramp, in ordinary
// context, which saves the FP registers and calls rl_fm_slow (memory_switch.cpp: decodes the access, runs it
// through rdSlow/wrSlow, writes the loaded value into the record, moves the record's pc past the instruction).
// rdSlow may run guest code (a VM page fault), and that may fault again: each fault has its own record, so they
// nest. The trampoline then executes a udf at rl_fm_tramp_exit; that exception restores the record's context
// (pstate included) and resumes the thread after the access. If rl_fm_slow cannot decode the instruction, the
// instruction runs again unchanged and its second fault goes to libnx's handler (the crash report) with the
// original context (g_rlFmPassPc). Every other exception goes to libnx's code, copied unchanged below.
//
// Record (FmRec in memory_switch.cpp): x0-x30 at 0x000, sp 0x0F8, pc 0x100, pstate 0x108, far 0x110, esr 0x118,
// q0-q31 0x120, fpsr 0x320, fpcr 0x328. It sits 0x380 bytes below the faulting sp (AArch64 has no red zone).

    // weak, as in libnx: the port may not define a C handler (then crashes go to the kernel as before)
    .weak __libnx_exception_handler

    .section .bss.rl_fm, "aw", %nobits
    .balign 8
    .global g_rlFmPassPc
g_rlFmPassPc:
    .skip 8

    .section .text.rl_fm_entry, "ax", %progbits
    .balign 4
    .global __libnx_exception_entry
    .type __libnx_exception_entry, %function
__libnx_exception_entry:
    cbz     x1, .Lorig
    ldr     x2, [x1, #0x58]                 // pc
    adrp    x3, rl_fm_tramp_exit
    add     x3, x3, :lo12:rl_fm_tramp_exit
    cmp     x2, x3
    b.eq    .Lexit
    cmp     w0, #0x101                      // data abort
    b.ne    .Lorig
    adrp    x3, g_rlFmBase
    ldr     x3, [x3, :lo12:g_rlFmBase]
    cbz     x3, .Lorig
    ldr     x4, [x1, #0x70]                 // far
    sub     x4, x4, x3
    lsr     x4, x4, #32
    cbnz    x4, .Lorig                      // not in the window
    adrp    x3, g_rlFmPassPc
    ldr     x4, [x3, :lo12:g_rlFmPassPc]
    cmp     x4, x2
    b.ne    1f
    str     xzr, [x3, :lo12:g_rlFmPassPc]
    b       .Lorig                          // rl_fm_slow could not run it: report it
1:  ldr     x5, [x1, #0x50]                 // the record, below the thread's sp
    sub     x5, x5, #0x380
    and     x5, x5, #~15
    ldp     x6, x7, [x1, #0x00]
    stp     x6, x7, [x5, #0x00]
    ldp     x6, x7, [x1, #0x10]
    stp     x6, x7, [x5, #0x10]
    ldp     x6, x7, [x1, #0x20]
    stp     x6, x7, [x5, #0x20]
    ldp     x6, x7, [x1, #0x30]
    stp     x6, x7, [x5, #0x30]
    ldr     x6, [x1, #0x40]
    str     x6, [x5, #0x40]
    stp     x9, x10, [x5, #0x48]
    stp     x11, x12, [x5, #0x58]
    stp     x13, x14, [x5, #0x68]
    stp     x15, x16, [x5, #0x78]
    stp     x17, x18, [x5, #0x88]
    stp     x19, x20, [x5, #0x98]
    stp     x21, x22, [x5, #0xA8]
    stp     x23, x24, [x5, #0xB8]
    stp     x25, x26, [x5, #0xC8]
    stp     x27, x28, [x5, #0xD8]
    str     x29, [x5, #0xE8]
    ldr     x6, [x1, #0x48]                 // lr
    str     x6, [x5, #0xF0]
    ldr     x6, [x1, #0x50]                 // sp
    str     x6, [x5, #0xF8]
    str     x2, [x5, #0x100]                // pc
    ldr     w6, [x1, #0x60]                 // pstate
    str     x6, [x5, #0x108]
    ldr     x6, [x1, #0x70]                 // far
    str     x6, [x5, #0x110]
    ldr     w6, [x1, #0x6C]                 // esr
    str     x6, [x5, #0x118]
    str     x5, [x1, #0x00]                 // resume in the trampoline: x0 = sp = the record
    str     x5, [x1, #0x50]
    adrp    x6, rl_fm_tramp
    add     x6, x6, :lo12:rl_fm_tramp
    str     x6, [x1, #0x58]
    mov     w0, #0
    bl      svcReturnFromException
    b       .

.Lexit:                                     // the trampoline is done; x19 is its record
    mov     x5, x19
    ldp     x6, x7, [x5, #0x00]
    stp     x6, x7, [x1, #0x00]
    ldp     x6, x7, [x5, #0x10]
    stp     x6, x7, [x1, #0x10]
    ldp     x6, x7, [x5, #0x20]
    stp     x6, x7, [x1, #0x20]
    ldp     x6, x7, [x5, #0x30]
    stp     x6, x7, [x1, #0x30]
    ldr     x6, [x5, #0x40]
    str     x6, [x1, #0x40]
    ldr     x6, [x5, #0xF0]
    str     x6, [x1, #0x48]                 // lr
    ldr     x6, [x5, #0xF8]
    str     x6, [x1, #0x50]                 // sp
    ldr     x6, [x5, #0x100]
    str     x6, [x1, #0x58]                 // pc
    ldr     x6, [x5, #0x108]
    str     w6, [x1, #0x60]                 // pstate
    ldp     x9, x10, [x5, #0x48]
    ldp     x11, x12, [x5, #0x58]
    ldp     x13, x14, [x5, #0x68]
    ldp     x15, x16, [x5, #0x78]
    ldp     x17, x18, [x5, #0x88]
    ldp     x19, x20, [x5, #0x98]
    ldp     x21, x22, [x5, #0xA8]
    ldp     x23, x24, [x5, #0xB8]
    ldp     x25, x26, [x5, #0xC8]
    ldp     x27, x28, [x5, #0xD8]
    ldr     x29, [x5, #0xE8]
    mov     w0, #0
    bl      svcReturnFromException
    b       .

// libnx's __libnx_exception_entry (libnx/source/runtime/exception.s), unchanged: crash reports and the port's
// __libnx_exception_handler keep working for every exception that is not ours.
.Lorig:
    cmp     x1, #0
    b.eq    .Lnx_abort
    adrp    x5, :got:__libnx_exception_handler
    ldr     x5, [x5, #:got_lo12:__libnx_exception_handler]
    cmp     x5, #0
    b.eq    .Lnx_abort
    stp     x9, x10, [sp, #-16]!
    stp     x11, x12, [sp, #-16]!
    stp     x13, x14, [sp, #-16]!
    stp     x15, x16, [sp, #-16]!
    stp     x17, x18, [sp, #-16]!
    stp     x19, x20, [sp, #-16]!
    str     x21, [sp, #-16]!
    stp     x0, x1, [sp, #-16]!
    sub     sp, sp, #16
    mov     x0, sp
    mov     x1, #8
    mov     w2, wzr
    mov     x3, #0
    bl      svcGetInfo
    mov     w6, w0
    ldr     x7, [sp], #16
    ldp     x0, x1, [sp], #16
    ldr     x21, [sp], #16
    ldp     x19, x20, [sp], #16
    ldp     x17, x18, [sp], #16
    ldp     x15, x16, [sp], #16
    ldp     x13, x14, [sp], #16
    ldp     x11, x12, [sp], #16
    ldp     x9, x10, [sp], #16
    cbnz    w6, .Lnx_abort
    adrp    x6, __nx_exception_ignoredebug
    ldr     w5, [x6, #:lo12:__nx_exception_ignoredebug]
    cbnz    w5, .Lnx_start
    cbnz    x7, .Lnx_abort
.Lnx_start:
    adrp    x2, __nx_exceptiondump
    add     x2, x2, #:lo12:__nx_exceptiondump
    mov     x5, x2
    str     w0, [x2], #4
    str     wzr, [x2], #4
    str     wzr, [x2], #4
    str     wzr, [x2], #4
    ldp     x3, x4, [x1]
    str     x5, [x1], #16
    stp     x3, x4, [x2], #16
    ldp     x3, x4, [x1], #16
    stp     x3, x4, [x2], #16
    ldp     x3, x4, [x1], #16
    stp     x3, x4, [x2], #16
    ldp     x3, x4, [x1], #16
    stp     x3, x4, [x2], #16
    ldr     x3, [x1], #8
    str     x3, [x2], #8
    str     x9, [x2], #8
    stp     x10, x11, [x2], #16
    stp     x12, x13, [x2], #16
    stp     x14, x15, [x2], #16
    stp     x16, x17, [x2], #16
    stp     x18, x19, [x2], #16
    stp     x20, x21, [x2], #16
    stp     x22, x23, [x2], #16
    stp     x24, x25, [x2], #16
    stp     x26, x27, [x2], #16
    str     x28, [x2], #8
    str     x29, [x2], #8
    ldr     x3, [x1], #8
    str     x3, [x2], #8
    adrp    x4, __nx_exception_stack
    add     x4, x4, #:lo12:__nx_exception_stack
    adrp    x5, __nx_exception_stack_size
    ldr     x5, [x5, #:lo12:__nx_exception_stack_size]
    add     x4, x4, x5
    ldr     x3, [x1]
    str     x4, [x1], #8
    str     x3, [x2], #8
    adrp    x4, __libnx_exception_returnentry
    add     x4, x4, #:lo12:__libnx_exception_returnentry
    ldr     x3, [x1]
    str     x4, [x1], #8
    str     x3, [x2], #8
    str     xzr, [x2], #8
    stp     q0, q1, [x2], #32
    stp     q2, q3, [x2], #32
    stp     q4, q5, [x2], #32
    stp     q6, q7, [x2], #32
    stp     q8, q9, [x2], #32
    stp     q10, q11, [x2], #32
    stp     q12, q13, [x2], #32
    stp     q14, q15, [x2], #32
    stp     q16, q17, [x2], #32
    stp     q18, q19, [x2], #32
    stp     q20, q21, [x2], #32
    stp     q22, q23, [x2], #32
    stp     q24, q25, [x2], #32
    stp     q26, q27, [x2], #32
    stp     q28, q29, [x2], #32
    stp     q30, q31, [x2], #32
    ldr     w3, [x1], #4
    str     w3, [x2], #4
    ldr     w3, [x1], #4
    str     w3, [x2], #4
    ldr     w3, [x1], #4
    str     w3, [x2], #4
    ldr     w3, [x1], #4
    str     w3, [x2], #4
    ldr     x3, [x1], #8
    str     x3, [x2], #8
    mov     w0, wzr
    b       .Lnx_end
.Lnx_abort:
    mov     w0, #0xf801
.Lnx_end:
    bl      svcReturnFromException
    b       .
    .size __libnx_exception_entry, . - __libnx_exception_entry

// The trampoline, in ordinary context: x0 = sp = the record (16-aligned), every other register the faulting
// thread's own.
    .balign 4
    .global rl_fm_tramp
    .type rl_fm_tramp, %function
rl_fm_tramp:
    mov     x19, x0                         // (x19 itself is in the record)
    add     x2, x19, #0x120
    stp     q0, q1, [x2, #0x000]
    stp     q2, q3, [x2, #0x020]
    stp     q4, q5, [x2, #0x040]
    stp     q6, q7, [x2, #0x060]
    stp     q8, q9, [x2, #0x080]
    stp     q10, q11, [x2, #0x0A0]
    stp     q12, q13, [x2, #0x0C0]
    stp     q14, q15, [x2, #0x0E0]
    stp     q16, q17, [x2, #0x100]
    stp     q18, q19, [x2, #0x120]
    stp     q20, q21, [x2, #0x140]
    stp     q22, q23, [x2, #0x160]
    stp     q24, q25, [x2, #0x180]
    stp     q26, q27, [x2, #0x1A0]
    stp     q28, q29, [x2, #0x1C0]
    stp     q30, q31, [x2, #0x1E0]
    mrs     x3, fpsr
    mrs     x4, fpcr
    add     x5, x19, #0x320
    stp     x3, x4, [x5]
    mov     x0, x19
    bl      rl_fm_slow
    cbnz    w0, 1f
    adrp    x3, g_rlFmPassPc                // not handled: its next fault is reported
    ldr     x4, [x19, #0x100]
    str     x4, [x3, :lo12:g_rlFmPassPc]
1:  add     x5, x19, #0x320
    ldp     x3, x4, [x5]
    msr     fpsr, x3
    msr     fpcr, x4
    add     x2, x19, #0x120
    ldp     q0, q1, [x2, #0x000]
    ldp     q2, q3, [x2, #0x020]
    ldp     q4, q5, [x2, #0x040]
    ldp     q6, q7, [x2, #0x060]
    ldp     q8, q9, [x2, #0x080]
    ldp     q10, q11, [x2, #0x0A0]
    ldp     q12, q13, [x2, #0x0C0]
    ldp     q14, q15, [x2, #0x0E0]
    ldp     q16, q17, [x2, #0x100]
    ldp     q18, q19, [x2, #0x120]
    ldp     q20, q21, [x2, #0x140]
    ldp     q22, q23, [x2, #0x160]
    ldp     q24, q25, [x2, #0x180]
    ldp     q26, q27, [x2, #0x1A0]
    ldp     q28, q29, [x2, #0x1C0]
    ldp     q30, q31, [x2, #0x1E0]
    .global rl_fm_tramp_exit
rl_fm_tramp_exit:
    udf     #0
    .size rl_fm_tramp, . - rl_fm_tramp
