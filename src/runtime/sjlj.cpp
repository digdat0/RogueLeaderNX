#include "sjlj.h"

#if defined(_WIN64)

// rcx = buffer (Win64 calling convention)
extern "C" __attribute__((naked, returns_twice)) int rt_setjmp(rt::JmpBuf*) {
    asm volatile(
        "movq %rbx, 0(%rcx)\n\t"
        "movq %rbp, 8(%rcx)\n\t"
        "movq %rdi, 16(%rcx)\n\t"
        "movq %rsi, 24(%rcx)\n\t"
        "leaq 8(%rsp), %rdx\n\t"
        "movq %rdx, 32(%rcx)\n\t"
        "movq %r12, 40(%rcx)\n\t"
        "movq %r13, 48(%rcx)\n\t"
        "movq %r14, 56(%rcx)\n\t"
        "movq %r15, 64(%rcx)\n\t"
        "movq (%rsp), %rdx\n\t"
        "movq %rdx, 72(%rcx)\n\t"
        "movdqa %xmm6, 96(%rcx)\n\t"
        "movdqa %xmm7, 112(%rcx)\n\t"
        "movdqa %xmm8, 128(%rcx)\n\t"
        "movdqa %xmm9, 144(%rcx)\n\t"
        "movdqa %xmm10, 160(%rcx)\n\t"
        "movdqa %xmm11, 176(%rcx)\n\t"
        "movdqa %xmm12, 192(%rcx)\n\t"
        "movdqa %xmm13, 208(%rcx)\n\t"
        "movdqa %xmm14, 224(%rcx)\n\t"
        "movdqa %xmm15, 240(%rcx)\n\t"
        "xorl %eax, %eax\n\t"
        "retq\n\t");
}

// rcx = buffer, edx = value (0 becomes 1)
extern "C" __attribute__((naked)) void rt_longjmp(rt::JmpBuf*, int) {
    asm volatile(
        "movl %edx, %eax\n\t"
        "testl %eax, %eax\n\t"
        "jnz 1f\n\t"
        "movl $1, %eax\n\t"
        "1:\n\t"
        "movq 0(%rcx), %rbx\n\t"
        "movq 8(%rcx), %rbp\n\t"
        "movq 16(%rcx), %rdi\n\t"
        "movq 24(%rcx), %rsi\n\t"
        "movq 40(%rcx), %r12\n\t"
        "movq 48(%rcx), %r13\n\t"
        "movq 56(%rcx), %r14\n\t"
        "movq 64(%rcx), %r15\n\t"
        "movdqa 96(%rcx), %xmm6\n\t"
        "movdqa 112(%rcx), %xmm7\n\t"
        "movdqa 128(%rcx), %xmm8\n\t"
        "movdqa 144(%rcx), %xmm9\n\t"
        "movdqa 160(%rcx), %xmm10\n\t"
        "movdqa 176(%rcx), %xmm11\n\t"
        "movdqa 192(%rcx), %xmm12\n\t"
        "movdqa 208(%rcx), %xmm13\n\t"
        "movdqa 224(%rcx), %xmm14\n\t"
        "movdqa 240(%rcx), %xmm15\n\t"
        "movq 32(%rcx), %rsp\n\t"
        "jmpq *72(%rcx)\n\t");
}

#elif defined(__x86_64__)

// System V: rdi = buffer
extern "C" __attribute__((naked, returns_twice)) int rt_setjmp(rt::JmpBuf*) {
    asm volatile(
        "movq %rbx, 0(%rdi)\n\t"
        "movq %rbp, 8(%rdi)\n\t"
        "leaq 8(%rsp), %rdx\n\t"
        "movq %rdx, 32(%rdi)\n\t"
        "movq %r12, 40(%rdi)\n\t"
        "movq %r13, 48(%rdi)\n\t"
        "movq %r14, 56(%rdi)\n\t"
        "movq %r15, 64(%rdi)\n\t"
        "movq (%rsp), %rdx\n\t"
        "movq %rdx, 72(%rdi)\n\t"
        "xorl %eax, %eax\n\t"
        "retq\n\t");
}

// rdi = buffer, esi = value (0 becomes 1)
extern "C" __attribute__((naked)) void rt_longjmp(rt::JmpBuf*, int) {
    asm volatile(
        "movl %esi, %eax\n\t"
        "testl %eax, %eax\n\t"
        "jnz 1f\n\t"
        "movl $1, %eax\n\t"
        "1:\n\t"
        "movq 0(%rdi), %rbx\n\t"
        "movq 8(%rdi), %rbp\n\t"
        "movq 40(%rdi), %r12\n\t"
        "movq 48(%rdi), %r13\n\t"
        "movq 56(%rdi), %r14\n\t"
        "movq 64(%rdi), %r15\n\t"
        "movq 32(%rdi), %rsp\n\t"
        "jmpq *72(%rdi)\n\t");
}

#elif defined(__aarch64__)

// AAPCS64: x0 = buffer. Layout (sjlj.h): x19-x28 at 0-72, x29 at 80, x30 at 88, sp at 96,
// d8-d15 at 112-168. The return address (x30) is where rt_longjmp continues.
extern "C" __attribute__((naked, returns_twice)) int rt_setjmp(rt::JmpBuf*) {
    asm volatile(
        "stp x19, x20, [x0, #0]\n\t"
        "stp x21, x22, [x0, #16]\n\t"
        "stp x23, x24, [x0, #32]\n\t"
        "stp x25, x26, [x0, #48]\n\t"
        "stp x27, x28, [x0, #64]\n\t"
        "stp x29, x30, [x0, #80]\n\t"
        "mov x9, sp\n\t"
        "str x9, [x0, #96]\n\t"
        "stp d8, d9, [x0, #112]\n\t"
        "stp d10, d11, [x0, #128]\n\t"
        "stp d12, d13, [x0, #144]\n\t"
        "stp d14, d15, [x0, #160]\n\t"
        "mov w0, #0\n\t"
        "ret\n\t");
}

// x0 = buffer, w1 = value (0 becomes 1)
extern "C" __attribute__((naked)) void rt_longjmp(rt::JmpBuf*, int) {
    asm volatile(
        "ldp x19, x20, [x0, #0]\n\t"
        "ldp x21, x22, [x0, #16]\n\t"
        "ldp x23, x24, [x0, #32]\n\t"
        "ldp x25, x26, [x0, #48]\n\t"
        "ldp x27, x28, [x0, #64]\n\t"
        "ldp x29, x30, [x0, #80]\n\t"
        "ldr x9, [x0, #96]\n\t"
        "mov sp, x9\n\t"
        "ldp d8, d9, [x0, #112]\n\t"
        "ldp d10, d11, [x0, #128]\n\t"
        "ldp d12, d13, [x0, #144]\n\t"
        "ldp d14, d15, [x0, #160]\n\t"
        "cmp w1, #0\n\t"
        "csinc w0, w1, wzr, ne\n\t"
        "ret\n\t");  // (to x30: a RET is not checked by branch target identification)
}

#else
#error "sjlj.cpp: only x86-64 (Win64 and System V) and AArch64 are implemented"
#endif
