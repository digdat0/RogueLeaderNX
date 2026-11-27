// RL-NX (Stage 11): FASTMEM's address window (-DRL_NX_FASTMEM builds only; src/runtime/memory_switch.cpp memInit).
//
// A 4 GiB range of address space is reserved (libnx virtmem: nothing else is placed there) and two pieces of memory
// are mapped into it, each through a code memory object (svcCreateCodeMemory + MapOwner with read/write, the mapping
// libnx's JIT uses; the heap pages behind it become inaccessible at their old address):
//   window + 0x80000000  the guest RAM arena (with its slack)
//   window + 0xE0000000  the locked cache
// Every other page of the window is unmapped, so a recompiled F access (ctx.h rd32F...: window + guest address)
// that is not plain RAM or the locked cache faults; fastmem_entry.S hands it to rl_fm_slow (memory_switch.cpp).
// The mappings lock heap pages, so a relaunch through hbloader must undo them first (rlnx_fastmem_teardown, from
// clang_tls_switch.c's kartpad_exit_to_loader); a plain exit ends the process and the kernel frees everything.
#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
unsigned long long g_rlFmBase = 0;  // the window (fastmem_entry.S reads it), 0 until mapped
}

namespace {
constexpr unsigned long long kWindow = 1ull << 32;
struct Piece {
    void* src = nullptr;
    void* dst = nullptr;
    size_t size = 0;
    Handle h = INVALID_HANDLE;
};
Piece s_ram, s_lc;
VirtmemReservation* s_resv = nullptr;
char s_err[160] = "";

bool mapPiece(Piece& p, void* dst, size_t size) {
    p.size = (size + 0xFFF) & ~size_t(0xFFF);
    p.src = aligned_alloc(0x1000, p.size);
    if (!p.src) {
        snprintf(s_err, sizeof s_err, "no memory for %zu bytes", p.size);
        return false;
    }
    Result rc = svcCreateCodeMemory(&p.h, p.src, p.size);
    if (R_FAILED(rc)) {
        snprintf(s_err, sizeof s_err, "svcCreateCodeMemory(%zu bytes) rc 0x%x", p.size, rc);
        free(p.src);
        p.src = nullptr;
        return false;
    }
    rc = svcControlCodeMemory(p.h, CodeMapOperation_MapOwner, dst, p.size, Perm_Rw);
    if (R_FAILED(rc)) {
        snprintf(s_err, sizeof s_err, "svcControlCodeMemory(MapOwner %p, %zu bytes) rc 0x%x", dst, p.size, rc);
        svcCloseHandle(p.h);
        p.h = INVALID_HANDLE;
        free(p.src);
        p.src = nullptr;
        return false;
    }
    p.dst = dst;
    return true;
}
void unmapPiece(Piece& p) {
    if (p.dst) svcControlCodeMemory(p.h, CodeMapOperation_UnmapOwner, p.dst, p.size, Perm_None);
    if (p.h != INVALID_HANDLE) svcCloseHandle(p.h);
    if (p.src) free(p.src);
    p = Piece{};
}
}  // namespace

// Maps the window. ram/lc get the arena's and the locked cache's new addresses. On failure *why says what went wrong
// (the caller stops: a fastmem gen cannot run without the window).
extern "C" bool rlnx_fastmem_map(unsigned char** ram, size_t ramSize, unsigned char** lc, size_t lcSize, const char** why) {
    virtmemLock();
    void* w = virtmemFindAslr(kWindow, 0x10000);
    if (!w) {
        virtmemUnlock();
        *why = "no free 4 GiB of address space";
        return false;
    }
    s_resv = virtmemAddReservation(w, kWindow);
    const bool ok = s_resv && mapPiece(s_ram, static_cast<char*>(w) + 0x80000000ull, ramSize) &&
                    mapPiece(s_lc, static_cast<char*>(w) + 0xE0000000ull, lcSize);
    if (!ok) {
        unmapPiece(s_ram);
        if (s_resv) virtmemRemoveReservation(s_resv);
        s_resv = nullptr;
        virtmemUnlock();
        *why = s_err[0] ? s_err : "virtmemAddReservation failed";
        return false;
    }
    virtmemUnlock();
    std::memset(s_ram.dst, 0, s_ram.size);
    std::memset(s_lc.dst, 0, s_lc.size);
    *ram = static_cast<unsigned char*>(s_ram.dst);
    *lc = static_cast<unsigned char*>(s_lc.dst);
    g_rlFmBase = reinterpret_cast<unsigned long long>(w);
    return true;
}

// Relaunch (kartpad_exit_to_loader): give the heap pages back before hbloader loads the next NRO.
extern "C" void rlnx_fastmem_teardown(void) {
    if (!g_rlFmBase) return;
    g_rlFmBase = 0;
    unmapPiece(s_lc);
    unmapPiece(s_ram);
    virtmemLock();
    if (s_resv) virtmemRemoveReservation(s_resv);
    s_resv = nullptr;
    virtmemUnlock();
}
