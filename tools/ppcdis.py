#!/usr/bin/env python3
"""Small Gekko (PowerPC 750CL) disassembler for exploring main.dol.

  python tools/ppcdis.py <main.dol> <addr> [count]      # disassemble
  python tools/ppcdis.py <main.dol> --refs <addr>       # who references addr (bl/b/lis+addi/lis+lwz pairs)

Library use: Dol(path).word(addr), dis(word, addr) -> text.
"""
import struct
import sys


class Dol:
    def __init__(self, path):
        d = open(path, "rb").read()
        self.data = d
        offs = struct.unpack(">18I", d[0:0x48])
        addrs = struct.unpack(">18I", d[0x48:0x90])
        sizes = struct.unpack(">18I", d[0x90:0xD8])
        self.bss, self.bss_size, self.entry = struct.unpack(">III", d[0xD8:0xE4])
        self.sections = []  # (name, addr, size, file_off, is_text)
        for i in range(18):
            if sizes[i]:
                name = f"T{i}" if i < 7 else f"D{i - 7}"
                self.sections.append((name, addrs[i], sizes[i], offs[i], i < 7))

    def section(self, addr):
        for s in self.sections:
            if s[1] <= addr < s[1] + s[2]:
                return s
        return None

    def word(self, addr):
        s = self.section(addr)
        if not s:
            return None
        o = s[3] + addr - s[1]
        return struct.unpack(">I", self.data[o:o + 4])[0]

    def read(self, addr, n):
        s = self.section(addr)
        o = s[3] + addr - s[1]
        return self.data[o:o + n]

    def text_words(self):
        for name, a, sz, off, is_text in self.sections:
            if is_text:
                for k in range(0, sz, 4):
                    yield a + k, struct.unpack(">I", self.data[off + k:off + k + 4])[0]


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


SPR = {1: "xer", 8: "lr", 9: "ctr", 18: "dsisr", 19: "dar", 22: "dec", 25: "sdr1", 26: "srr0", 27: "srr1",
       272: "sprg0", 273: "sprg1", 274: "sprg2", 275: "sprg3", 282: "ear", 268: "tbl", 269: "tbu", 284: "tbl_w", 285: "tbu_w",
       912: "gqr0", 913: "gqr1", 914: "gqr2", 915: "gqr3", 916: "gqr4", 917: "gqr5", 918: "gqr6", 919: "gqr7",
       920: "hid2", 921: "wpar", 922: "dmau", 923: "dmal", 1008: "hid0", 1009: "hid1", 1017: "l2cr"}
for _i in range(4):
    SPR[528 + 2 * _i] = f"ibat{_i}u"; SPR[529 + 2 * _i] = f"ibat{_i}l"
    SPR[536 + 2 * _i] = f"dbat{_i}u"; SPR[537 + 2 * _i] = f"dbat{_i}l"


def dis(w, pc):
    op = w >> 26
    rD = (w >> 21) & 31
    rA = (w >> 16) & 31
    rB = (w >> 11) & 31
    imm = w & 0xFFFF
    simm = sx16(imm)
    rc = "." if w & 1 else ""
    if op == 18:
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        tgt = li if w & 2 else (pc + li) & 0xFFFFFFFF
        return ("bl" if w & 1 else "b") + ("a" if w & 2 else "") + f" 0x{tgt:08x}"
    if op == 16:
        bd = w & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        tgt = bd if w & 2 else (pc + bd) & 0xFFFFFFFF
        return f"bc{'l' if w & 1 else ''} {rD},{rA},0x{tgt:08x}"
    if op == 19:
        xo = (w >> 1) & 0x3FF
        names = {16: "bclr", 528: "bcctr", 50: "rfi", 150: "isync", 0: "mcrf", 257: "crand", 449: "cror", 193: "crxor",
                 225: "crnand", 33: "crnor", 289: "creqv", 129: "crandc", 417: "crorc"}
        n = names.get(xo, f"op19_{xo}")
        if xo in (16, 528):
            if rD == 20:
                return ("blr" if xo == 16 else "bctr") + ("l" if w & 1 else "")
            return f"{n}{'l' if w & 1 else ''} {rD},{rA}"
        if xo in (50, 150):
            return n
        return f"{n} {rD},{rA},{rB}"
    if op == 17:
        return "sc"
    D = {14: "addi", 15: "addis", 12: "addic", 13: "addic.", 8: "subfic", 7: "mulli", 24: "ori", 25: "oris", 26: "xori",
         27: "xoris", 28: "andi.", 29: "andis.", 10: "cmpli", 11: "cmpi", 3: "twi",
         32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau", 36: "stw", 37: "stwu",
         38: "stb", 39: "stbu", 44: "sth", 45: "sthu", 46: "lmw", 47: "stmw", 48: "lfs", 49: "lfsu", 50: "lfd", 51: "lfdu",
         52: "stfs", 53: "stfsu", 54: "stfd", 55: "stfdu"}
    if op in D:
        n = D[op]
        if op in (14, 15) and rA == 0:
            return f"{'li' if op == 14 else 'lis'} r{rD},{simm if op == 14 else hex(imm)}"
        if op in (24, 25, 26, 27, 28, 29):
            return f"{n} r{rA},r{rD},0x{imm:x}"
        if op in (10, 11):
            crf = rD >> 2
            v = imm if op == 10 else simm
            return f"{'cmplwi' if op == 10 else 'cmpwi'} cr{crf},r{rA},{v}"
        if op >= 32:
            fp = "f" if 48 <= op <= 55 else "r"
            return f"{n} {fp}{rD},{simm}(r{rA})"
        return f"{n} r{rD},r{rA},{simm}"
    if op in (56, 57, 60, 61):  # psq_l/lu/st/stu
        n = {56: "psq_l", 57: "psq_lu", 60: "psq_st", 61: "psq_stu"}[op]
        d12 = w & 0xFFF
        if d12 & 0x800:
            d12 -= 0x1000
        return f"{n} f{rD},{d12}(r{rA}),{(w >> 15) & 1},qr{(w >> 12) & 7}"
    if op == 20 or op == 21 or op == 23:
        sh, mb, me = rB, (w >> 6) & 31, (w >> 1) & 31
        n = {20: "rlwimi", 21: "rlwinm", 23: "rlwnm"}[op]
        return f"{n}{rc} r{rA},r{rD},{('r' + str(sh)) if op == 23 else sh},{mb},{me}"
    if op == 31:
        xo = (w >> 1) & 0x3FF
        if xo == 467 or xo == 339:
            spr = rA | (rB << 5)
            s = SPR.get(spr, str(spr))
            return f"mtspr {s},r{rD}" if xo == 467 else f"mfspr r{rD},{s}"
        if xo == 371:
            spr = rA | (rB << 5)
            return f"mftb r{rD},{spr}"
        X = {266: "add", 10: "addc", 138: "adde", 234: "addme", 202: "addze", 40: "subf", 8: "subfc", 136: "subfe",
             232: "subfme", 200: "subfze", 104: "neg", 235: "mullw", 75: "mulhw", 11: "mulhwu", 491: "divw", 459: "divwu"}
        xo9 = (w >> 1) & 0x1FF
        if xo9 in X:
            o = "o" if w & 0x400 else ""
            return f"{X[xo9]}{o}{rc} r{rD},r{rA},r{rB}"
        L = {23: "lwzx", 55: "lwzux", 87: "lbzx", 119: "lbzux", 279: "lhzx", 311: "lhzux", 343: "lhax", 375: "lhaux",
             151: "stwx", 183: "stwux", 215: "stbx", 247: "stbux", 407: "sthx", 439: "sthux", 534: "lwbrx", 662: "stwbrx",
             790: "lhbrx", 918: "sthbrx", 535: "lfsx", 567: "lfsux", 599: "lfdx", 631: "lfdux", 663: "stfsx", 695: "stfsux",
             727: "stfdx", 759: "stfdux", 983: "stfiwx", 20: "lwarx", 150: "stwcx.", 597: "lswi", 725: "stswi", 533: "lswx", 661: "stswx"}
        if xo in L:
            fp = "f" if "f" in L[xo][:3] else "r"
            return f"{L[xo]} {fp}{rD},r{rA},r{rB}"
        A = {28: "and", 60: "andc", 444: "or", 412: "orc", 316: "xor", 476: "nand", 124: "nor", 284: "eqv", 24: "slw",
             536: "srw", 792: "sraw", 824: "srawi", 26: "cntlzw", 954: "extsb", 922: "extsh"}
        if xo in A:
            if xo == 444 and rD == rB:
                return f"mr{rc} r{rA},r{rD}"
            if xo in (26, 954, 922):
                return f"{A[xo]}{rc} r{rA},r{rD}"
            if xo == 824:
                return f"srawi{rc} r{rA},r{rD},{rB}"
            return f"{A[xo]}{rc} r{rA},r{rD},r{rB}"
        if xo == 0:
            return f"cmpw cr{rD >> 2},r{rA},r{rB}"
        if xo == 32:
            return f"cmplw cr{rD >> 2},r{rA},r{rB}"
        if xo == 19:
            return f"mfcr r{rD}"
        if xo == 144:
            return f"mtcrf 0x{(w >> 12) & 0xFF:x},r{rD}"
        if xo == 83:
            return f"mfmsr r{rD}"
        if xo == 146:
            return f"mtmsr r{rD}"
        if xo == 210:
            return f"mtsr {rA & 15},r{rD}"
        if xo == 242:
            return f"mtsrin r{rD},r{rB}"
        if xo == 595:
            return f"mfsr r{rD},{rA & 15}"
        if xo == 512:
            return f"mcrxr cr{rD >> 2}"
        C = {86: "dcbf", 54: "dcbst", 470: "dcbi", 278: "dcbt", 246: "dcbtst", 1014: "dcbz", 982: "icbi"}
        if xo in C:
            return f"{C[xo]} r{rA},r{rB}"
        S = {598: "sync", 854: "eieio", 306: "tlbie", 566: "tlbsync"}
        if xo in S:
            return S[xo] + (f" r{rB}" if xo == 306 else "")
        if xo == 4:
            return f"tw {rD},r{rA},r{rB}"
        return f"op31_{xo} r{rD},r{rA},r{rB}"
    if op == 59 or op == 63:
        xo5 = (w >> 1) & 31
        rC = (w >> 6) & 31
        s = "s" if op == 59 else ""
        F5 = {18: "fdiv", 20: "fsub", 21: "fadd", 22: "fsqrt", 24: "fres", 25: "fmul", 26: "frsqrte", 28: "fmsub",
              29: "fmadd", 30: "fnmsub", 31: "fnmadd", 23: "fsel"}
        if xo5 in F5:
            n = F5[xo5] + s + rc
            if xo5 == 25:
                return f"{n} f{rD},f{rA},f{rC}"
            if xo5 in (28, 29, 30, 31, 23):
                return f"{n} f{rD},f{rA},f{rC},f{rB}"
            if xo5 in (24, 26, 22):
                return f"{n} f{rD},f{rB}"
            return f"{n} f{rD},f{rA},f{rB}"
        xo = (w >> 1) & 0x3FF
        F = {0: "fcmpu", 32: "fcmpo", 12: "frsp", 14: "fctiw", 15: "fctiwz", 40: "fneg", 72: "fmr", 136: "fnabs", 264: "fabs",
             583: "mffs", 711: "mtfsf", 70: "mtfsb0", 38: "mtfsb1", 134: "mtfsfi", 64: "mcrfs"}
        if xo in F:
            if xo in (0, 32):
                return f"{F[xo]} cr{rD >> 2},f{rA},f{rB}"
            return f"{F[xo]}{rc} f{rD},f{rB}"
        return f"op{op}_{xo}"
    if op == 4:
        xo = (w >> 1) & 0x3FF
        xo5 = (w >> 1) & 31
        rC = (w >> 6) & 31
        P5 = {10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1", 14: "ps_madds0", 15: "ps_madds1", 18: "ps_div",
              20: "ps_sub", 21: "ps_add", 23: "ps_sel", 24: "ps_res", 25: "ps_mul", 26: "ps_rsqrte", 28: "ps_msub",
              29: "ps_madd", 30: "ps_nmsub", 31: "ps_nmadd"}
        P = {0: "ps_cmpu0", 32: "ps_cmpo0", 64: "ps_cmpu1", 96: "ps_cmpo1", 40: "ps_neg", 72: "ps_mr", 136: "ps_nabs",
             264: "ps_abs", 528: "ps_merge00", 560: "ps_merge01", 592: "ps_merge10", 624: "ps_merge11", 1014: "dcbz_l"}
        if xo in P:
            return f"{P[xo]} f{rD},f{rA},f{rB}"
        if xo5 in P5:
            return f"{P5[xo5]} f{rD},f{rA},f{rC},f{rB}"
        if xo5 in (6, 7):
            return f"psq_l{'u' if xo & 32 else ''}x f{rD},r{rA},r{rB}" if xo5 == 6 else f"psq_st{'u' if xo & 32 else ''}x f{rD},r{rA},r{rB}"
        return f"op4_{xo}"
    if op == 0 and w == 0:
        return ".word 0"
    return f".word 0x{w:08x}"


def main(argv):
    dol = Dol(argv[1])
    if argv[2] == "--refs":
        tgt = int(argv[3], 16)
        hi = {}
        for a, w in dol.text_words():
            op = w >> 26
            if op in (18, 16):
                if op == 18:
                    li = w & 0x03FFFFFC
                    if li & 0x02000000: li -= 0x04000000
                    t = (li if w & 2 else a + li) & 0xFFFFFFFF
                else:
                    bd = w & 0xFFFC
                    if bd & 0x8000: bd -= 0x10000
                    t = (bd if w & 2 else a + bd) & 0xFFFFFFFF
                if t == tgt:
                    print(f"{a:08x}  {dis(w, a)}")
            if op == 15:
                hi[(w >> 21) & 31] = ((w & 0xFFFF) << 16, a)
            elif op in (14, 32, 34, 40, 42, 36, 38, 44, 48, 50, 52, 54) and ((w >> 16) & 31) in hi:
                base, at = hi[(w >> 16) & 31]
                if a - at < 64 and (base + sx16(w & 0xFFFF)) & 0xFFFFFFFF == tgt:
                    print(f"{a:08x}  {dis(w, a)}   (hi @{at:08x})")
        return 0
    start = int(argv[2], 16)
    n = int(argv[3]) if len(argv) > 3 else 32
    for i in range(n):
        a = start + i * 4
        w = dol.word(a)
        if w is None:
            break
        print(f"{a:08x}  {w:08x}  {dis(w, a)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
