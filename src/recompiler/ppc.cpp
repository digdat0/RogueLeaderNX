#include "ppc.h"

namespace ppc {

static bool valid31(u32 w) {
    switch (xo10(w)) {
    case 0: case 4: case 8: case 10: case 11: case 19: case 20: case 23: case 24: case 26: case 28: case 32:
    case 40: case 54: case 55: case 60: case 75: case 83: case 86: case 87: case 104: case 119: case 124:
    case 136: case 138: case 144: case 146: case 150: case 151: case 183: case 200: case 202: case 210:
    case 215: case 232: case 234: case 235: case 242: case 246: case 247: case 266: case 278: case 279:
    case 284: case 306: case 310: case 311: case 316: case 339: case 343: case 371: case 375: case 407:
    case 412: case 438: case 439: case 444: case 459: case 467: case 470: case 476: case 491: case 512:
    case 533: case 534: case 535: case 536: case 566: case 567: case 595: case 597: case 598: case 599:
    case 631: case 659: case 661: case 662: case 663: case 695: case 725: case 727: case 759: case 790:
    case 792: case 824: case 854: case 918: case 922: case 954: case 982: case 983: case 1014:
    // OE forms
    case 520: case 522: case 552: case 616: case 648: case 650: case 712: case 714: case 744: case 746:
    case 747: case 778: case 971: case 1003:
        return true;
    default:
        return false;
    }
}

static bool valid4(u32 w) {
    switch (xo10(w)) {
    case 0: case 32: case 64: case 96: case 40: case 72: case 136: case 264: case 528: case 560: case 592:
    case 624: case 1014:
        return true;
    }
    switch (xo5(w)) {
    case 10: case 11: case 12: case 13: case 14: case 15: case 18: case 20: case 21: case 23: case 24:
    case 25: case 26: case 28: case 29: case 30: case 31:
        return true;
    }
    switch ((w >> 1) & 0x3F) {
    case 6: case 7: case 38: case 39:
        return true;
    }
    return false;
}

bool isValid(u32 w) {
    switch (op(w)) {
    case 3: case 7: case 8: case 10: case 11: case 12: case 13: case 14: case 15: case 16: case 17: case 18:
    case 20: case 21: case 23: case 24: case 25: case 26: case 27: case 28: case 29:
    case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39: case 40: case 41: case 42:
    case 43: case 44: case 45: case 46: case 47: case 48: case 49: case 50: case 51: case 52: case 53:
    case 54: case 55: case 56: case 57: case 60: case 61:
        return true;
    case 4:
        return valid4(w);
    case 19:
        switch (xo10(w)) {
        case 0: case 16: case 33: case 50: case 129: case 150: case 193: case 225: case 257: case 289:
        case 417: case 449: case 528:
            return true;
        }
        return false;
    case 31:
        return valid31(w);
    case 59:
        switch (xo5(w)) {
        case 18: case 20: case 21: case 22: case 24: case 25: case 28: case 29: case 30: case 31:
            return true;
        }
        return false;
    case 63:
        switch (xo5(w)) {
        case 18: case 20: case 21: case 22: case 23: case 25: case 26: case 28: case 29: case 30: case 31:
            return true;
        }
        switch (xo10(w)) {
        case 0: case 12: case 14: case 15: case 32: case 38: case 40: case 64: case 70: case 72: case 134:
        case 136: case 264: case 583: case 711:
            return true;
        }
        return false;
    default:
        return false;
    }
}

Flow flowOf(u32 w) {
    if (!isValid(w)) return Flow::Illegal;
    switch (op(w)) {
    case 18:
        return LK(w) ? Flow::Call : Flow::Branch;
    case 16: {
        if (LK(w)) return Flow::Call;
        // BO = 1z1zz: branch always
        return (BO(w) & 0x14) == 0x14 ? Flow::Branch : Flow::CondBranch;
    }
    case 19:
        switch (xo10(w)) {
        case 16:
            if (LK(w)) return Flow::IndirectCall;
            return (BO(w) & 0x14) == 0x14 ? Flow::Return : Flow::CondReturn;
        case 528:
            if (LK(w)) return Flow::IndirectCall;
            return (BO(w) & 0x14) == 0x14 ? Flow::IndirectJump : Flow::CondIndirect;
        case 50:
            return Flow::Rfi;
        }
        return Flow::Normal;
    default:
        return Flow::Normal;
    }
}

}  // namespace ppc
