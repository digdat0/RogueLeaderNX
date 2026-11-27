// RL-NX: booting the game from the disc, for the Switch.
//
// This is src/runtime/main.cpp's `loadFst` + `bootDisc` with the same byte layout, because both are
// static in the desktop app that this port parks. The IPL/apploader low-memory block below is the
// part that has to be exactly right: the game reads its disc ID, arena and FST pointers from it
// before its own startup runs, and a wrong value shows up as a crash inside Nintendo's startup code
// rather than in ours.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>

#include "runtime.h"  // rt::discRead, rt::hp, rt::wr32, rt::kMem1Size, rt::logf
#include "savestate.h"

namespace rt {
namespace {

struct DiscFile {
    std::string path;
    u32 off, size;
};

std::vector<DiscFile> s_files;

inline u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }

// The file system table, so a disc offset can be named in a log line ("boot/main.dol+0x1234").
void loadFst() {
    u8 hdr[0x440];
    if (!discRead(0, hdr, sizeof hdr)) return;
    const u32 fstOff = be32(hdr + 0x424), fstSize = be32(hdr + 0x428);
    std::vector<u8> fst(fstSize);
    if (!discRead(fstOff, fst.data(), fstSize)) return;
    const u32 n = be32(&fst[8]);
    const char* names = reinterpret_cast<const char*>(&fst[n * 12]);
    std::vector<std::pair<std::string, u32>> dirs{{"", n}};
    for (u32 i = 1; i < n; i++) {
        while (dirs.size() > 1 && i >= dirs.back().second) dirs.pop_back();
        const u8* e = &fst[i * 12];
        const std::string name = names + (be32(e) & 0xFFFFFF);
        const std::string path = dirs.back().first + name;
        if (e[0]) dirs.push_back({path + "/", be32(e + 8)});
        else s_files.push_back({path, be32(e + 4), be32(e + 8)});
    }
    logf("boot", "%u files on the disc", unsigned(s_files.size()));
}

}  // namespace

std::string discFileAt(u64 off) {
    for (auto& f : s_files)
        if (off >= f.off && off < u64(f.off) + f.size) {
            char b[32];
            snprintf(b, sizeof b, "+0x%llX", (unsigned long long)(off - f.off));
            return f.path + b;
        }
    return "";
}

// Load main.dol into guest memory and leave low memory the way the IPL and apploader do.
u32 bootDiscSwitch() {
    u8 hdr[0x440];
    if (!discRead(0, hdr, sizeof hdr)) fatal("cannot read the disc header");
    if (memcmp(hdr, "GSWE64", 6) != 0) fatal("this is not Star Wars: Rogue Leader (USA): game ID %.6s", (const char*)hdr);
    if (hdr[7] != 0) fatal("unsupported disc revision %u (rev 0 is supported)", hdr[7]);
    const u32 dolOff = be32(hdr + 0x420), fstOff = be32(hdr + 0x424), fstSize = be32(hdr + 0x428),
              fstMax = be32(hdr + 0x42C);

    u8 dh[0x100];
    discRead(dolOff, dh, sizeof dh);
    for (int i = 0; i < 18; i++) {
        const u32 off = be32(dh + 4 * i), addr = be32(dh + 0x48 + 4 * i), size = be32(dh + 0x90 + 4 * i);
        if (!size) continue;
        if (!discRead(dolOff + off, hp(addr), size)) fatal("cannot read DOL section %d", i);
    }
    const u32 entry = be32(dh + 0xE0);

    // The FST sits at the top of MEM1, as the apploader leaves it.
    const u32 fstAddr = (0x81800000u - fstMax) & ~0x1Fu;
    if (!discRead(fstOff, hp(fstAddr), fstSize)) fatal("cannot read the FST");
    const u32 bi2Addr = (fstAddr - 0x2000) & ~0x1Fu;
    discRead(0x440, hp(bi2Addr), 0x2000);

    memcpy(hp(0x80000000), hdr, 0x20);  // disc ID
    wr32(0x80000020, 0x0D15EA5E);       // boot magic
    wr32(0x80000024, 1);
    wr32(0x80000028, kMem1Size);        // physical memory size (24 MiB, as the game expects)
    wr32(0x8000002C, 3);                // console type: retail
    wr32(0x80000030, 0);                // arena low: from the linker symbol
    wr32(0x80000034, bi2Addr);          // arena high: below bi2/FST
    wr32(0x80000038, fstAddr);
    wr32(0x8000003C, fstMax);
    wr32(0x800000CC, 0);                // video mode: NTSC
    wr32(0x800000D0, 0x01000000);       // ARAM size
    wr32(0x800000EC, 0x81800000);       // debug monitor top (none)
    wr32(0x800000F0, kMem1Size);        // simulated memory size
    wr32(0x800000F4, bi2Addr);
    wr32(0x800000F8, u32(kBusClock));
    wr32(0x800000FC, u32(kCpuClock));
    logf("boot", "DOL entry %08X, FST at %08X, arena high %08X", entry, fstAddr, bi2Addr);
    loadFst();
    return entry;
}

// The disc is the player's own: the first .iso/.gcm/.rvz/.wia in the port's folder on the SD card.
// Naming it game.iso is the simple case; anything else in the folder is picked up in name order, so
// a card with one image in it just works.
std::string findDiscSwitch() {
    const char* kDirs[] = {"sdmc:/switch/RL-NX", "sdmc:/switch", "sdmc:/"};
    const char* kExts[] = {".iso", ".gcm", ".rvz", ".wia"};
    std::string best;
    for (const char* dir : kDirs) {
        DIR* d = opendir(dir);
        if (!d) continue;
        std::string found;
        while (dirent* e = readdir(d)) {
            const char* dot = strrchr(e->d_name, '.');
            if (!dot) continue;
            for (const char* ext : kExts) {
                if (strcasecmp(dot, ext)) continue;
                const std::string path = std::string(dir) + "/" + e->d_name;
                if (found.empty() || path < found) found = path;  // stable pick, not readdir order
            }
        }
        closedir(d);
        if (!found.empty()) {
            best = found;
            break;  // the port's own folder wins over the card root
        }
    }
    return best;
}

}  // namespace rt
