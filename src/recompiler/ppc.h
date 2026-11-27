// PowerPC (Gekko) instruction field helpers and a small classifier used by the analysis.
#pragma once
#include <cstdint>

namespace ppc {

using u32 = uint32_t;
using s32 = int32_t;

inline u32 op(u32 w) { return w >> 26; }
inline u32 rD(u32 w) { return (w >> 21) & 31; }
inline u32 rS(u32 w) { return (w >> 21) & 31; }
inline u32 rA(u32 w) { return (w >> 16) & 31; }
inline u32 rB(u32 w) { return (w >> 11) & 31; }
inline u32 rC(u32 w) { return (w >> 6) & 31; }
inline u32 uimm(u32 w) { return w & 0xFFFF; }
inline s32 simm(u32 w) { return int16_t(w & 0xFFFF); }
inline u32 xo10(u32 w) { return (w >> 1) & 0x3FF; }
inline u32 xo9(u32 w) { return (w >> 1) & 0x1FF; }
inline u32 xo5(u32 w) { return (w >> 1) & 0x1F; }
inline bool rc(u32 w) { return w & 1; }
inline bool oe(u32 w) { return (w >> 10) & 1; }
inline u32 BO(u32 w) { return (w >> 21) & 31; }
inline u32 BI(u32 w) { return (w >> 16) & 31; }
inline bool LK(u32 w) { return w & 1; }
inline bool AA(u32 w) { return (w >> 1) & 1; }
inline u32 spr(u32 w) { return ((w >> 16) & 31) | (((w >> 11) & 31) << 5); }
inline u32 crfD(u32 w) { return (w >> 23) & 7; }
inline u32 crfS(u32 w) { return (w >> 18) & 7; }

// Branch target of an I-form (b/bl) instruction at pc.
inline u32 bTarget(u32 w, u32 pc) {
    s32 li = s32(w & 0x03FFFFFC);
    if (li & 0x02000000) li -= 0x04000000;
    return AA(w) ? u32(li) : u32(pc + li);
}
// Branch target of a B-form (bc) instruction at pc.
inline u32 bcTarget(u32 w, u32 pc) {
    s32 bd = int16_t(w & 0xFFFC);
    return AA(w) ? u32(bd) : u32(pc + bd);
}

inline u32 rotMask(u32 mb, u32 me) {
    const u32 begin = 0xFFFFFFFFu >> mb;
    const u32 end = me < 31 ? (0xFFFFFFFFu >> (me + 1)) : 0;
    const u32 m = begin ^ end;
    return mb <= me ? m : ~m;
}

// Control-flow kind used by the analysis.
enum class Flow {
    Normal,      // falls through
    Branch,      // b (unconditional, no link)
    CondBranch,  // bc without link
    Call,        // bl / bcl
    Return,      // blr (unconditional)
    CondReturn,  // bclr conditional
    IndirectJump,// bctr (unconditional, no link)
    CondIndirect,// bcctr conditional, no link
    IndirectCall,// bctrl / blrl
    Rfi,         // rfi
    Illegal,     // not a valid instruction
};

bool isValid(u32 w);  // conservative: false for opcodes Gekko does not implement
Flow flowOf(u32 w);

}  // namespace ppc
