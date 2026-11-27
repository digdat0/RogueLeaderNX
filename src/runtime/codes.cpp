// Cheat codes and patches in Dolphin's GameSettings format (codes.ini next to settings.ini),
// applied once per retrace like Dolphin applies them once per field:
//   [OnFrame]       patches: "0x80001234:dword:0x12345678[:expected]" (byte / word / dword)
//   [ActionReplay]  decrypted Action Replay codes ("XXXXXXXX YYYYYYYY"): RAM writes and fills,
//                   pointer writes, increments, conditionals, fill & slide, memory copy
//   [Gecko]         Gecko codes: RAM writes, string and serial writes, 16/32-bit conditionals,
//                   base address / pointer offset codes, repeats, gotos and subroutines, Gecko
//                   registers, address range checks, terminators and endif/else
// and the _Enabled / _Disabled lists of each kind. The game's code is recompiled ahead of time,
// so codes that change instructions (Gecko C2 "insert ASM", C6 branches, writes into the game's
// code) cannot work here; they are listed as unsupported and never run. So are codes that run
// PowerPC code of their own (C0) and the rarer types this engine does not implement.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>

#include "mods.h"
#include "runtime.h"

namespace rt::mods {
namespace {

std::mutex s_mutex;
std::vector<Code> s_codes;

// ---------------------------------------------------------------------------------------
// guest memory: MEM1 and VM pages the game has mapped; never faults, ignores anything else
// ---------------------------------------------------------------------------------------
u8* memPtr(u32 a, bool write) {
    if (isMem1(a)) return hp(a);
    if (isVm(a)) {
        u8* p = (write ? g_vmWrite : g_vmRead)[(a - kVmStart) >> 12];
        return p ? p + (a & 0xFFF) : nullptr;
    }
    return nullptr;
}
u32 read8(u32 a) {
    const u8* p = memPtr(a, false);
    return p ? *p : 0;
}
u32 read16(u32 a) { return read8(a) << 8 | read8(a + 1); }
u32 read32(u32 a) { return read16(a) << 16 | read16(a + 2); }
void write8(u32 a, u32 v) {
    if (u8* p = memPtr(a, true)) *p = u8(v);
}
void write16(u32 a, u32 v) {
    write8(a, v >> 8);
    write8(a + 1, v);
}
void write32(u32 a, u32 v) {
    write16(a, v >> 16);
    write16(a + 2, v);
}
float asFloat(u32 v) {
    float f;
    memcpy(&f, &v, 4);
    return f;
}
u32 asBits(float f) {
    u32 v;
    memcpy(&v, &f, 4);
    return v;
}

// Where Rogue Leader (GSWE64) keeps code: the exception vectors, the main DOL's text, and the
// game's text in its virtual memory. (0x80001800-0x80003000, where Dolphin puts its code
// handler, is free memory here: writes there are data.)
bool isCodeAddr(u32 a) {
    if (a >= 0xC0000000u && a < 0xC1800000u) a -= 0x40000000u;
    return (a >= 0x80000100u && a < 0x80001800u) || (a >= 0x80003100u && a < 0x8005C360u) ||
           (a >= 0x7FC00000u && a < 0x7FD9E2F0u);
}

std::string hex8(u32 v) {
    char b[12];
    snprintf(b, sizeof b, "%08X", v);
    return b;
}

// "XXXXXXXX YYYYYYYY" (spaces optional): false for anything else (encrypted AR codes, junk)
bool parsePair(const std::string& line, u32& a, u32& b) {
    std::string h;
    for (char ch : line)
        if (!std::isspace(static_cast<unsigned char>(ch))) h += ch;
    if (h.size() != 16) return false;
    for (char ch : h)
        if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
    a = u32(strtoul(h.substr(0, 8).c_str(), nullptr, 16));
    b = u32(strtoul(h.substr(8).c_str(), nullptr, 16));
    return true;
}

std::vector<std::pair<u32, u32>> pairs(const Code& c, bool& ok) {
    std::vector<std::pair<u32, u32>> out;
    ok = true;
    for (const auto& l : c.lines) {
        u32 a, b;
        if (!parsePair(l, a, b)) {
            ok = false;
            break;
        }
        out.emplace_back(a, b);
    }
    return out;
}

// ---------------------------------------------------------------------------------------
// Dolphin patches ([OnFrame])
// ---------------------------------------------------------------------------------------
struct Patch {
    u32 addr = 0, size = 0, value = 0, expected = 0;
    bool conditional = false;
};
bool parsePatch(const std::string& line, Patch& p) {
    std::vector<std::string> f;
    size_t start = 0;
    for (;;) {
        const size_t c = line.find(':', start);
        f.push_back(line.substr(start, c == std::string::npos ? std::string::npos : c - start));
        if (c == std::string::npos) break;
        start = c + 1;
    }
    if (f.size() < 3 || f.size() > 4) return false;
    auto num = [](std::string s, u32& v) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
        if (s.empty()) return false;
        char* end = nullptr;
        v = u32(strtoul(s.c_str(), &end, 16));
        return end && *end == 0;
    };
    std::string t = f[1];
    for (auto& ch : t) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.erase(t.begin());
    p.size = t == "byte" ? 1 : t == "word" ? 2 : t == "dword" ? 4 : 0;
    if (!p.size || !num(f[0], p.addr) || !num(f[2], p.value)) return false;
    p.conditional = f.size() == 4;
    return !p.conditional || num(f[3], p.expected);
}

void runPatch(const Code& c) {
    for (const auto& l : c.lines) {
        Patch p;
        if (!parsePatch(l, p)) continue;
        const u32 cur = p.size == 1 ? read8(p.addr) : p.size == 2 ? read16(p.addr) : read32(p.addr);
        if (p.conditional && cur != p.expected) continue;
        if (p.size == 1) write8(p.addr, p.value);
        else if (p.size == 2) write16(p.addr, p.value);
        else write32(p.addr, p.value);
    }
}

// ---------------------------------------------------------------------------------------
// Action Replay (decrypted): address word = subtype:2 type:3 size:2 address:25
// ---------------------------------------------------------------------------------------
u32 arAddr(u32 w) { return (w & 0x01FFFFFFu) | 0x80000000u; }

bool arCompare(u32 type, u32 size, u32 a, u32 v) {
    u32 cur = 0;
    switch (size) {
    case 0: cur = read8(a); v &= 0xFF; break;
    case 1: cur = read16(a); v &= 0xFFFF; break;
    default: cur = read32(a); break;
    }
    if (size == 3) {  // float
        const float x = asFloat(cur), y = asFloat(v);
        switch (type) {
        case 1: return x == y;
        case 2: return x != y;
        case 3: case 5: return x < y;
        case 4: case 6: return x > y;
        case 7: return (cur & v) != 0;
        default: return false;
        }
    }
    auto sx = [&](u32 x) { return size == 0 ? s32(s8(x)) : size == 1 ? s32(s16(x)) : s32(x); };
    switch (type) {
    case 1: return cur == v;
    case 2: return cur != v;
    case 3: return sx(cur) < sx(v);
    case 4: return sx(cur) > sx(v);
    case 5: return cur < v;
    case 6: return cur > v;
    case 7: return (cur & v) != 0;
    default: return false;
    }
}

void runActionReplay(const Code& c) {
    bool ok;
    const auto ops = pairs(c, ok);
    if (!ok) return;
    int skip = 0;           // > 0: lines to skip; -1: until "00000000 40000000"; -2: the rest
    bool fillSlide = false, memCopy = false;
    u32 last = 0;
    for (const auto& [w, v] : ops) {
        if (skip) {
            if (skip > 0) skip--;
            else if (skip == -2) return;
            else if (w == 0 && v == 0x40000000u) skip = 0;
            continue;
        }
        if (fillSlide) {  // 00000000 8XXXXXXX (last: size, address), then value / increments
            fillSlide = false;
            const u32 size = (last >> 25) & 3;
            u32 a = arAddr(last), val = w;
            const s32 addrInc = s16(v & 0xFFFF);
            const s32 valInc = s8(v >> 24);
            const u32 n = (v >> 16) & 0xFF;
            for (u32 i = 0; i < n; i++) {
                if (size == 0) write8(a, val), a += u32(addrInc);
                else if (size == 1) write16(a, val), a += u32(addrInc * 2);
                else write32(a, val), a += u32(addrInc * 4);
                val += u32(valInc);
            }
            continue;
        }
        if (memCopy) {
            memCopy = false;
            const u32 dst = arAddr(last), src = arAddr(w), n = v & 0x7FFF;
            if ((v >> 24) == 0)
                for (u32 i = 0; i < n; i++) write8(dst + i, read8(src + i));
            continue;
        }
        if (w == 0) {  // zero codes
            switch (v >> 29) {
            case 0: return;  // end of codes
            case 2: break;   // normal execution (also "00000000 40000000", the endif)
            case 4:
                if (((v >> 25) & 3) == 3) memCopy = true;
                else fillSlide = true;
                last = v;
                break;
            default: return;
            }
            continue;
        }
        const u32 size = (w >> 25) & 3, type = (w >> 27) & 7, sub = (w >> 30) & 3;
        const u32 a = arAddr(w);
        if (type == 0) {
            switch (sub) {
            case 0:  // RAM write (and fill)
                if (size == 0)
                    for (u32 i = 0; i <= (v >> 8); i++) write8(a + i, v);
                else if (size == 1)
                    for (u32 i = 0; i <= (v >> 16); i++) write16(a + 2 * i, v);
                else
                    write32(a, v);
                break;
            case 1: {  // write through a pointer
                const u32 p = read32(a);
                if (size == 0) write8(p + (v >> 8), v);
                else if (size == 1) write16(p + ((v >> 16) << 1), v);
                else write32(p, v);
                break;
            }
            case 2:  // add
                if (size == 0) write8(a, read8(a) + v);
                else if (size == 1) write16(a, read16(a) + v);
                else if (size == 2) write32(a, read32(a) + v);
                else write32(a, asBits(asFloat(read32(a)) + float(v)));
                break;
            default: break;  // master codes
            }
            continue;
        }
        if (!arCompare(type, size, a, v)) {
            switch (sub) {
            case 0: skip = 1; break;
            case 1: skip = 2; break;
            case 2: skip = -1; break;
            default: skip = -2; break;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------
// Gecko
// ---------------------------------------------------------------------------------------
u32 s_gr[16];  // Gecko registers (kept across frames, like the code handler's)

struct GeckoBlock {
    u32 line = 0, count = 0;
};

// lines a code occupies after its first (data of string writes, serial writes, ASM blocks)
u32 geckoExtraLines(u32 op, u32 w0, u32 w1) {
    switch (op) {
    case 0x06: return (w1 + 7) / 8;
    case 0x08: return 1;
    case 0xC0: case 0xC2: case 0xC4: case 0xF2: case 0xF4: return w1;
    case 0xF6: return w0 & 0xFF;
    default: return 0;
    }
}

u32 geckoOp(u32 w0) {
    const u32 t = w0 >> 24;
    return t < 0x40 ? (t & 0xEE) : (t & 0xFE);
}

void runGecko(const Code& c) {
    bool ok;
    const auto L = pairs(c, ok);
    if (!ok) return;
    u32 ba = 0x80000000u, po = 0x80000000u;
    u32 skip = 0;  // bit 0: the current block does not execute (bit n: n ifs up)
    GeckoBlock blocks[16];
    u32 budget = 100000;  // repeats and gotos must not hang the game
    for (u32 i = 0; i < L.size() && budget; budget--) {
        const u32 w0 = L[i].first, w1 = L[i].second;
        const u32 op = geckoOp(w0);
        const bool usePo = (w0 >> 24) < 0x40 && (w0 & 0x10000000u);
        const u32 base = usePo ? po : ba;
        const u32 addr = base + (w0 & 0x01FFFFFFu);
        const bool exec = (skip & 1) == 0;
        const u32 next = i + 1 + geckoExtraLines(op, w0, w1);
        auto pushIf = [&](bool cond) { skip = (skip << 1) | ((skip & 1) || !cond ? 1u : 0u); };
        // 4x / 8x: address + (ba | po) + grN
        auto regAddr = [&](u32 x) {
            u32 a = x;
            const u32 y = (w0 >> 16) & 0xF, z = (w0 >> 12) & 0xF;
            if (y == 1) a += ba;
            else if (y == 2) a += po;
            if (z == 1) a += s_gr[w0 & 0xF];
            return a;
        };
        auto flowCond = [&] {
            const u32 t = (w0 >> 20) & 0xF;
            return t == 2 || (t == 0 && exec) || (t == 1 && !exec);
        };
        switch (op) {
        case 0x00:  // 8-bit write and fill
            if (exec)
                for (u32 k = 0; k <= (w1 >> 16); k++) write8(addr + k, w1);
            break;
        case 0x02:  // 16-bit write and fill
            if (exec)
                for (u32 k = 0; k <= (w1 >> 16); k++) write16(addr + 2 * k, w1);
            break;
        case 0x04:
            if (exec) write32(addr, w1);
            break;
        case 0x06:  // string write
            if (exec)
                for (u32 k = 0; k < w1; k++) {
                    const auto& [a, b] = L[std::min<size_t>(i + 1 + k / 8, L.size() - 1)];
                    const u32 word = (k & 4) ? b : a;
                    write8(addr + k, word >> (24 - 8 * (k & 3)));
                }
            break;
        case 0x08:  // serial write: TNNNZZZZ VVVVVVVV
            if (exec && i + 1 < L.size()) {
                const u32 d0 = L[i + 1].first, d1 = L[i + 1].second;
                const u32 size = d0 >> 28, n = (d0 >> 16) & 0xFFF, inc = d0 & 0xFFFF;
                u32 a = addr, v = w1;
                for (u32 k = 0; k <= n; k++) {
                    if (size == 0) write8(a, v);
                    else if (size == 1) write16(a, v);
                    else write32(a, v);
                    a += inc;
                    v += d1;
                }
            }
            break;
        case 0x20: case 0x22: case 0x24: case 0x26: case 0x28: case 0x2A: case 0x2C: case 0x2E: {
            if (w0 & 1) skip >>= 1;  // endif, then this if
            const u32 a = addr & ~1u;
            bool cond = false;
            if ((skip & 1) == 0) {
                if (op <= 0x26) {
                    const u32 cur = read32(a);
                    cond = op == 0x20 ? cur == w1 : op == 0x22 ? cur != w1 : op == 0x24 ? cur > w1 : cur < w1;
                } else {
                    const u32 cur = read16(a) & ~(w1 >> 16) & 0xFFFF, val = w1 & 0xFFFF;
                    cond = op == 0x28 ? cur == val : op == 0x2A ? cur != val : op == 0x2C ? cur > val : cur < val;
                }
            }
            pushIf(cond);
            break;
        }
        case 0x40: case 0x48:  // load ba / po from memory
            if (exec) {
                const u32 v = read32(regAddr(w1));
                u32& r = op == 0x40 ? ba : po;
                r = ((w0 >> 20) & 0xF) == 1 ? r + v : v;
            }
            break;
        case 0x42: case 0x4A:  // set ba / po
            if (exec) {
                const u32 v = regAddr(w1);
                u32& r = op == 0x42 ? ba : po;
                r = ((w0 >> 20) & 0xF) == 1 ? r + v : v;
            }
            break;
        case 0x44: case 0x4C:  // store ba / po
            if (exec) write32(regAddr(w1), op == 0x44 ? ba : po);
            break;
        case 0x60:  // set repeat
            if (exec) blocks[w1 & 0xF] = {i + 1, w0 & 0xFFFF};
            break;
        case 0x62:  // execute repeat
            if (exec && blocks[w1 & 0xF].count) {
                blocks[w1 & 0xF].count--;
                i = blocks[w1 & 0xF].line;
                continue;
            }
            break;
        case 0x64:  // return
            if (flowCond()) {
                i = blocks[w1 & 0xF].line;
                continue;
            }
            break;
        case 0x66:  // goto
            if (flowCond()) {
                i = u32(s32(i + 1) + s16(w0 & 0xFFFF));
                continue;
            }
            break;
        case 0x68:  // gosub
            if (flowCond()) {
                blocks[w1 & 0xF].line = i + 1;
                i = u32(s32(i + 1) + s16(w0 & 0xFFFF));
                continue;
            }
            break;
        case 0x80:  // set / add grN
            if (exec) {
                u32 v = w1;
                const u32 y = (w0 >> 16) & 0xF;
                if (y == 1) v += ba;
                else if (y == 2) v += po;
                u32& r = s_gr[w0 & 0xF];
                r = ((w0 >> 20) & 0xF) == 1 ? r + v : v;
            }
            break;
        case 0x82: {  // load grN
            if (exec) {
                const u32 y = (w0 >> 16) & 0xF;
                const u32 a = w1 + (y == 1 ? ba : y == 2 ? po : 0);
                const u32 u = (w0 >> 20) & 0xF;
                s_gr[w0 & 0xF] = u == 0 ? read8(a) : u == 1 ? read16(a) : read32(a);
            }
            break;
        }
        case 0x84: {  // store grN (repeated)
            if (exec) {
                const u32 y = (w0 >> 16) & 0xF;
                const u32 a = w1 + (y == 1 ? ba : y == 2 ? po : 0);
                const u32 u = (w0 >> 20) & 0xF, n = (w0 >> 4) & 0xFFF;
                const u32 v = s_gr[w0 & 0xF];
                for (u32 k = 0; k <= n; k++) {
                    if (u == 0) write8(a + k, v);
                    else if (u == 1) write16(a + 2 * k, v);
                    else write32(a + 4 * k, v);
                }
            }
            break;
        }
        case 0x86: case 0x88: {  // grN = ([]grN) op ([]value / grM)
            if (exec) {
                const u32 t = (w0 >> 20) & 0xF, y = (w0 >> 16) & 0xF;
                u32& r = s_gr[w0 & 0xF];
                const u32 x = (y & 1) ? read32(r) : r;
                u32 v = op == 0x86 ? w1 : s_gr[w1 & 0xF];
                if (y & 2) v = read32(v);
                u32 res = x;
                switch (t) {
                case 0: res = x + v; break;
                case 1: res = x * v; break;
                case 2: res = x | v; break;
                case 3: res = x & v; break;
                case 4: res = x ^ v; break;
                case 5: res = (v & 63) >= 32 ? 0 : x << (v & 31); break;
                case 6: res = (v & 63) >= 32 ? 0 : x >> (v & 31); break;
                case 7: res = (x << (v & 31)) | (v & 31 ? x >> (32 - (v & 31)) : 0); break;
                case 8: res = u32(s32(x) >> std::min<u32>(v & 63, 31)); break;
                case 9: res = asBits(asFloat(x) + asFloat(v)); break;
                case 10: res = asBits(asFloat(x) * asFloat(v)); break;
                default: break;
                }
                r = res;
            }
            break;
        }
        case 0xCE: case 0xDE: {  // address range check (ba / po)
            const u32 v = op == 0xCE ? ba : po;
            bool cond = false;
            if ((skip & 1) == 0) cond = v >= (w1 & 0xFFFF0000u) && v < (w1 << 16);
            pushIf(cond);
            break;
        }
        case 0xE0:  // full terminator
            skip = 0;
            if (w1 >> 16) ba = w1 & 0xFFFF0000u;
            if (w1 & 0xFFFF) po = w1 << 16;
            break;
        case 0xE2: {  // endif (+ else)
            const u32 n = w0 & 0xFF;
            skip = n >= 32 ? 0 : skip >> n;
            if (((w0 >> 20) & 0xF) == 1 && (skip & 2) == 0) skip ^= 1;
            if (w1 >> 16) ba = w1 & 0xFFFF0000u;
            if (w1 & 0xFFFF) po = w1 << 16;
            break;
        }
        case 0xF0:  // end of the code list
            return;
        default:
            return;  // (classifyCode keeps other types from running)
        }
        i = next;
    }
}

// what a Gecko code needs that this engine cannot do ("" = fine)
std::string geckoProblem(const std::vector<std::pair<u32, u32>>& L) {
    u32 ba = 0x80000000u;
    bool baKnown = true;
    for (size_t i = 0; i < L.size();) {
        const u32 w0 = L[i].first, w1 = L[i].second;
        const u32 op = geckoOp(w0);
        const bool usePo = (w0 >> 24) < 0x40 && (w0 & 0x10000000u);
        switch (op) {
        case 0x00: case 0x02: case 0x04: case 0x06: case 0x08:
            if (!usePo && baKnown) {
                const u32 a = ba + (w0 & 0x01FFFFFFu);
                if (isCodeAddr(a)) return "It changes the game's code at " + hex8(a) + ", which is recompiled ahead of time.";
            }
            break;
        case 0x20: case 0x22: case 0x24: case 0x26: case 0x28: case 0x2A: case 0x2C: case 0x2E:
        case 0x40: case 0x44: case 0x48: case 0x4A: case 0x4C:
        case 0x60: case 0x62: case 0x64: case 0x66: case 0x68:
        case 0x80: case 0x82: case 0x84: case 0x86: case 0x88:
        case 0xCE: case 0xDE: case 0xE2: case 0xF0:
            if (op == 0x40) baKnown = false;
            break;
        case 0x42:  // a constant base address is followed; one read from memory is not
            if (((w0 >> 16) & 0xF) == 0 && ((w0 >> 12) & 0xF) == 0) ba = ((w0 >> 20) & 0xF) == 1 ? ba + w1 : w1;
            else baKnown = false;
            break;
        case 0xE0:
            if (w1 >> 16) ba = w1 & 0xFFFF0000u;
            baKnown = true;
            break;
        case 0xC2: case 0xC4: case 0xC6:
            return "It inserts code into the game (Gecko C2 / C6), which is recompiled ahead of time.";
        case 0xC0:
            return "It runs PowerPC code of its own (Gecko C0), which Red Two does not support.";
        default: {
            char b[96];
            snprintf(b, sizeof b, "It uses Gecko code type %02X, which Red Two does not support.", w0 >> 24);
            return b;
        }
        }
        i += 1 + geckoExtraLines(op, w0, w1);
    }
    return {};
}

std::string arProblem(const std::vector<std::pair<u32, u32>>& L) {
    for (size_t i = 0; i < L.size(); i++) {
        const u32 w = L[i].first, v = L[i].second;
        if (w == 0) {
            const u32 z = v >> 29;
            if (z == 3 || z > 4) return "It uses an Action Replay zero code Red Two does not support.";
            if (z == 4) i++;  // its data line
            continue;
        }
        const u32 type = (w >> 27) & 7, sub = (w >> 30) & 3;
        if (type == 0 && sub != 3 && isCodeAddr(arAddr(w)))
            return "It changes the game's code at " + hex8(arAddr(w)) + ", which is recompiled ahead of time.";
    }
    return {};
}

// --- INI ----------------------------------------------------------------------------------
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || std::isspace(static_cast<unsigned char>(s.back())))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
    return s.substr(i);
}

const char* kSection[3] = {"Gecko", "ActionReplay", "OnFrame"};

}  // namespace

void classifyCode(Code& c) {
    c.supported = true;
    c.why.clear();
    if (c.lines.empty()) {
        c.supported = false;
        c.why = "The code has no lines.";
        return;
    }
    if (c.kind == CodeKind::Patch) {
        for (const auto& l : c.lines) {
            Patch p;
            if (!parsePatch(l, p)) {
                c.supported = false;
                c.why = "Unreadable patch line: " + l;
                return;
            }
            if (isCodeAddr(p.addr)) {
                c.supported = false;
                c.why = "It changes the game's code at " + hex8(p.addr) + ", which is recompiled ahead of time.";
                return;
            }
        }
        return;
    }
    bool ok;
    const auto L = pairs(c, ok);
    if (!ok) {
        c.supported = false;
        c.why = c.kind == CodeKind::ActionReplay ? "Encrypted or unreadable Action Replay code (enter it decrypted, as Dolphin stores it)."
                                                 : "Unreadable code lines.";
        return;
    }
    c.why = c.kind == CodeKind::Gecko ? geckoProblem(L) : arProblem(L);
    c.supported = c.why.empty();
}

void parseCodeIni(const std::string& text, std::vector<Code>& out) {
    std::set<std::string> enabled[3], disabled[3];
    int kind = -1;         // the section's code kind
    int list = 0;          // 0 definitions, 1 _Enabled, 2 _Disabled
    Code* cur = nullptr;   // the definition being read
    std::vector<Code> defs;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty()) continue;
        if (line[0] == '[') {
            const std::string sec = line.substr(1, line.find(']') == std::string::npos ? std::string::npos : line.find(']') - 1);
            kind = -1;
            list = 0;
            cur = nullptr;
            for (int k = 0; k < 3; k++) {
                const std::string s = kSection[k];
                if (sec == s) kind = k;
                else if (sec == s + "_Enabled" || sec == s + "_RetroAchievements_Verified") kind = k, list = sec == s + "_Enabled" ? 1 : 3;
                else if (sec == s + "_Disabled") kind = k, list = 2;
            }
            continue;
        }
        if (kind < 0 || list == 3) continue;
        if (list) {
            if (line[0] == '$') (list == 1 ? enabled : disabled)[kind].insert(trim(line.substr(1)));
            continue;
        }
        if (line[0] == '$') {
            defs.emplace_back();
            cur = &defs.back();
            cur->kind = CodeKind(kind);
            std::string name = line.substr(1);
            if (kind == 0) {  // "$Name [creator]"
                const size_t b = name.find('[');
                if (b != std::string::npos) {
                    const size_t e = name.find(']', b);
                    cur->creator = trim(name.substr(b + 1, e == std::string::npos ? std::string::npos : e - b - 1));
                    name = name.substr(0, b);
                }
            }
            cur->name = trim(name);
            continue;
        }
        if (!cur) continue;
        if (line[0] == '*') {
            if (!cur->notes.empty()) cur->notes += ' ';
            cur->notes += trim(line.substr(1));
            continue;
        }
        cur->lines.push_back(line);
    }
    for (auto& d : defs) {
        classifyCode(d);
        auto it = std::find_if(out.begin(), out.end(), [&](const Code& o) { return o.kind == d.kind && o.name == d.name; });
        if (it != out.end()) {
            d.enabled = it->enabled;
            *it = std::move(d);
        } else {
            out.push_back(std::move(d));
        }
    }
    for (auto& c : out) {
        const int k = int(c.kind);
        if (enabled[k].count(c.name)) c.enabled = true;
        if (disabled[k].count(c.name)) c.enabled = false;
    }
}

namespace {

std::string serialize(const std::vector<Code>& list) {
    std::string s =
        "# Red Two cheat codes, in Dolphin's GameSettings format: Gecko codes under [Gecko], decrypted\n"
        "# Action Replay codes under [ActionReplay], patches (0x80001234:dword:0x12345678) under\n"
        "# [OnFrame]; each enabled code's $name under the matching _Enabled section. The in-game menu\n"
        "# (F1, MODS) switches them; Red Two applies the enabled ones every frame.\n";
    for (int k = 0; k < 3; k++) {
        bool any = false;
        for (const auto& c : list) any |= int(c.kind) == k;
        if (!any) continue;
        s += "\n[" + std::string(kSection[k]) + "]\n";
        for (const auto& c : list) {
            if (int(c.kind) != k) continue;
            s += "$" + c.name + (c.creator.empty() ? "" : " [" + c.creator + "]") + "\n";
            if (!c.notes.empty()) s += "*" + c.notes + "\n";
            for (const auto& l : c.lines) s += l + "\n";
        }
        s += "\n[" + std::string(kSection[k]) + "_Enabled]\n";
        for (const auto& c : list)
            if (int(c.kind) == k && c.enabled) s += "$" + c.name + "\n";
    }
    return s;
}

void saveLocked() {
    const std::string path = codesPath();
    if (path.empty()) return;
    if (FILE* f = fopen(path.c_str(), "wb")) {
        const std::string s = serialize(s_codes);
        fwrite(s.data(), 1, s.size(), f);
        fclose(f);
    } else {
        logf("mods", "cannot write %s", path.c_str());
    }
}

}  // namespace

std::string codesPath() {
    if (const char* p = getenv("R2_CODES")) return p;
    if (envFlag("R2_BACKGROUND") || envFlag("R2_HEADLESS") || getenv("R2_INPUT")) return {};
    const std::string u = userDir();
    return u.empty() ? std::string() : u + "codes.ini";
}

void codesInit() {
    const std::string path = codesPath();
    if (path.empty()) return;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    std::string text;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
    std::lock_guard lk(s_mutex);
    s_codes.clear();
    parseCodeIni(text, s_codes);
    size_t on = 0;
    for (const auto& c : s_codes) {
        on += c.enabled && c.supported;
        if (c.enabled && !c.supported) logf("mods", "code '%s' is enabled but cannot run here: %s", c.name.c_str(), c.why.c_str());
    }
    logf("mods", "%zu codes from %s (%zu enabled)", s_codes.size(), path.c_str(), on);
}

std::vector<Code> codesSnapshot() {
    std::lock_guard lk(s_mutex);
    return s_codes;
}

void setCodeEnabled(size_t index, bool on) {
    std::lock_guard lk(s_mutex);
    if (index >= s_codes.size()) return;
    s_codes[index].enabled = on;
    saveLocked();
}

void mergeCodes(const std::vector<Code>& add) {
    std::lock_guard lk(s_mutex);
    for (const auto& c : add) {
        auto it = std::find_if(s_codes.begin(), s_codes.end(), [&](const Code& o) { return o.kind == c.kind && o.name == c.name; });
        if (it != s_codes.end()) *it = c;
        else s_codes.push_back(c);
    }
    saveLocked();
}

void codesFrame() {
    std::unique_lock lk(s_mutex, std::try_to_lock);  // (never wait on the menu)
    if (!lk.owns_lock()) return;
    for (const auto& c : s_codes) {
        if (!c.enabled || !c.supported) continue;
        switch (c.kind) {
        case CodeKind::Patch: runPatch(c); break;
        case CodeKind::ActionReplay: runActionReplay(c); break;
        case CodeKind::Gecko: runGecko(c); break;
        }
    }
}

}  // namespace rt::mods
