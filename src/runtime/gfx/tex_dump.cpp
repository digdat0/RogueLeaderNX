// R2_TEX_DUMP=<directory>: every texture the game binds, decoded (level 0) to
// <directory>/<Dolphin replacement name>.png, once per name - the names a texture pack (or the
// port's own replacements) must use, palette hash included as Aurora computes it
// (texture_replacement.cpp: XXH64 of the palette range the texture's indices use).
#include "tex_dump.h"

#include "../runtime.h"

#include <xxhash.h>

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace rt::gfx::texdump {
namespace {

u16 be16(const u8* p) { return u16((p[0] << 8) | p[1]); }

struct Rgba {
    u8 r, g, b, a;
};

Rgba fromTlut(u16 v, u32 tlutFmt) {
    switch (tlutFmt) {
    case 0:  // IA8: alpha high byte, intensity low byte
        return {u8(v), u8(v), u8(v), u8(v >> 8)};
    case 1: {  // RGB565
        const u8 r = u8((v >> 11) & 31), g = u8((v >> 5) & 63), b = u8(v & 31);
        return {u8(r << 3 | r >> 2), u8(g << 2 | g >> 4), u8(b << 3 | b >> 2), 255};
    }
    default: {  // RGB5A3
        if (v & 0x8000) {
            const u8 r = u8((v >> 10) & 31), g = u8((v >> 5) & 31), b = u8(v & 31);
            return {u8(r << 3 | r >> 2), u8(g << 3 | g >> 2), u8(b << 3 | b >> 2), 255};
        }
        const u8 a = u8((v >> 12) & 7), r = u8((v >> 8) & 15), g = u8((v >> 4) & 15), b = u8(v & 15);
        return {u8(r * 17), u8(g * 17), u8(b * 17), u8(a << 5 | a << 2 | a >> 1)};
    }
    }
}

// level 0 to RGBA8 (row-major); false for formats it does not know
bool decode(u32 fmt, u32 w, u32 h, const u8* d, u32 tlutFmt, const u8* tlut, u32 tlutEntries, std::vector<Rgba>& out) {
    out.assign(size_t(w) * h, Rgba{0, 0, 0, 0});
    auto put = [&](u32 x, u32 y, Rgba c) {
        if (x < w && y < h) out[size_t(y) * w + x] = c;
    };
    auto pal = [&](u32 i) { return tlut && i < tlutEntries ? fromTlut(be16(tlut + 2 * i), tlutFmt) : Rgba{255, 0, 255, 255}; };
    u32 bw = 4, bh = 4;
    switch (fmt) {
    case 0: case 8: case 14: bw = 8; bh = 8; break;
    case 1: case 2: case 9: bw = 8; bh = 4; break;
    default: break;
    }
    const u32 bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;
    for (u32 ty = 0; ty < by; ty++)
        for (u32 tx = 0; tx < bx; tx++) {
            const u32 x0 = tx * bw, y0 = ty * bh;
            switch (fmt) {
            case 0:  // I4
            case 8:  // C4
                for (u32 y = 0; y < 8; y++)
                    for (u32 x = 0; x < 8; x++) {
                        const u8 v = d[y * 4 + x / 2];
                        const u32 n = x & 1 ? v & 15 : v >> 4;
                        put(x0 + x, y0 + y, fmt == 0 ? Rgba{u8(n * 17), u8(n * 17), u8(n * 17), u8(n * 17)} : pal(n));
                    }
                d += 32;
                break;
            case 1:  // I8
            case 9:  // C8
                for (u32 y = 0; y < 4; y++)
                    for (u32 x = 0; x < 8; x++) {
                        const u8 v = d[y * 8 + x];
                        put(x0 + x, y0 + y, fmt == 1 ? Rgba{v, v, v, v} : pal(v));
                    }
                d += 32;
                break;
            case 2:  // IA4
                for (u32 y = 0; y < 4; y++)
                    for (u32 x = 0; x < 8; x++) {
                        const u8 v = d[y * 8 + x], i = u8((v & 15) * 17), a = u8((v >> 4) * 17);
                        put(x0 + x, y0 + y, {i, i, i, a});
                    }
                d += 32;
                break;
            case 3:  // IA8
                for (u32 y = 0; y < 4; y++)
                    for (u32 x = 0; x < 4; x++) {
                        const u8 a = d[(y * 4 + x) * 2], i = d[(y * 4 + x) * 2 + 1];
                        put(x0 + x, y0 + y, {i, i, i, a});
                    }
                d += 32;
                break;
            case 4:  // RGB565
            case 5:  // RGB5A3
                for (u32 y = 0; y < 4; y++)
                    for (u32 x = 0; x < 4; x++) put(x0 + x, y0 + y, fromTlut(be16(d + (y * 4 + x) * 2), fmt == 4 ? 1 : 2));
                d += 32;
                break;
            case 6:  // RGBA8: AR pairs, then GB pairs
                for (u32 i = 0; i < 16; i++)
                    put(x0 + i % 4, y0 + i / 4, {d[i * 2 + 1], d[32 + i * 2], d[32 + i * 2 + 1], d[i * 2]});
                d += 64;
                break;
            case 10:  // C14X2
                for (u32 y = 0; y < 4; y++)
                    for (u32 x = 0; x < 4; x++) put(x0 + x, y0 + y, pal(be16(d + (y * 4 + x) * 2) & 0x3FFF));
                d += 32;
                break;
            case 14:  // CMPR: 2x2 DXT1 blocks per 8x8 tile
                for (u32 sb = 0; sb < 4; sb++) {
                    const u16 c0 = be16(d), c1 = be16(d + 2);
                    Rgba c[4] = {fromTlut(c0, 1), fromTlut(c1, 1)};
                    if (c0 > c1) {
                        c[2] = {u8((2 * c[0].r + c[1].r) / 3), u8((2 * c[0].g + c[1].g) / 3), u8((2 * c[0].b + c[1].b) / 3), 255};
                        c[3] = {u8((c[0].r + 2 * c[1].r) / 3), u8((c[0].g + 2 * c[1].g) / 3), u8((c[0].b + 2 * c[1].b) / 3), 255};
                    } else {
                        c[2] = {u8((c[0].r + c[1].r) / 2), u8((c[0].g + c[1].g) / 2), u8((c[0].b + c[1].b) / 2), 255};
                        c[3] = {0, 0, 0, 0};
                    }
                    for (u32 y = 0; y < 4; y++)
                        for (u32 x = 0; x < 4; x++)
                            put(x0 + (sb & 1) * 4 + x, y0 + (sb >> 1) * 4 + y, c[(d[4 + y] >> (6 - 2 * x)) & 3]);
                    d += 8;
                }
                break;
            default:
                return false;
            }
        }
    return true;
}

u32 crc32(const u8* p, size_t n, u32 crc = 0) {
    static u32 table[256];
    static bool init = false;
    if (!init) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void chunk(std::vector<u8>& f, const char* type, const std::vector<u8>& data) {
    const u32 n = u32(data.size());
    const u8 len[4] = {u8(n >> 24), u8(n >> 16), u8(n >> 8), u8(n)};
    f.insert(f.end(), len, len + 4);
    const size_t start = f.size();
    f.insert(f.end(), type, type + 4);
    f.insert(f.end(), data.begin(), data.end());
    const u32 c = crc32(f.data() + start, f.size() - start);
    const u8 cb[4] = {u8(c >> 24), u8(c >> 16), u8(c >> 8), u8(c)};
    f.insert(f.end(), cb, cb + 4);
}

// RGBA8 PNG with stored (uncompressed) deflate blocks
bool writePng(const std::filesystem::path& path, u32 w, u32 h, const std::vector<Rgba>& px) {
    std::vector<u8> raw;
    raw.reserve(size_t(h) * (w * 4 + 1));
    for (u32 y = 0; y < h; y++) {
        raw.push_back(0);
        const u8* row = reinterpret_cast<const u8*>(px.data() + size_t(y) * w);
        raw.insert(raw.end(), row, row + w * 4);
    }
    std::vector<u8> z = {0x78, 0x01};
    for (size_t off = 0; off < raw.size() || off == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        const bool last = off + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(u8(n));
        z.push_back(u8(n >> 8));
        z.push_back(u8(~n));
        z.push_back(u8(~n >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
        if (last) break;
    }
    u32 a = 1, b = 0;
    for (u8 c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    const u32 adler = (b << 16) | a;
    const u8 ab[4] = {u8(adler >> 24), u8(adler >> 16), u8(adler >> 8), u8(adler)};
    z.insert(z.end(), ab, ab + 4);
    std::vector<u8> f = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    const std::vector<u8> ihdr = {u8(w >> 24), u8(w >> 16), u8(w >> 8), u8(w), u8(h >> 24), u8(h >> 16), u8(h >> 8), u8(h), 8, 6, 0, 0, 0};
    chunk(f, "IHDR", ihdr);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    FILE* fp = nullptr;
#ifdef _WIN32
    if (_wfopen_s(&fp, path.wstring().c_str(), L"wb") != 0 || !fp) return false;
#else
    if (!(fp = fopen(path.c_str(), "wb"))) return false;
#endif
    fwrite(f.data(), 1, f.size(), fp);
    fclose(fp);
    return true;
}

u32 levelBytes(u32 fmt, u32 w, u32 h) {
    u32 bw = 4, bh = 4, bytes = 32;
    switch (fmt) {
    case 0: case 8: case 14: bw = 8; bh = 8; break;
    case 1: case 2: case 9: bw = 8; bh = 4; break;
    case 6: bytes = 64; break;
    default: break;
    }
    return ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes;
}

}  // namespace

bool decodeRgba(u32 fmt, u32 w, u32 h, const u8* data, u32 tlutFmt, const u8* tlut, u32 tlutEntries, std::vector<u8>& out) {
    std::vector<Rgba> px;
    if (!decode(fmt, w, h, data, tlutFmt, tlut, tlutEntries, px)) return false;
    out.resize(px.size() * 4);
    memcpy(out.data(), px.data(), out.size());
    return true;
}

bool writePng(const std::string& path, u32 w, u32 h, const u8* rgba) {
    std::vector<Rgba> px(size_t(w) * h);
    memcpy(px.data(), rgba, px.size() * 4);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    return writePng(std::filesystem::path(path), w, h, px);
}

// RL-NX: on the Switch the process working directory is the SD card root, so a relative R2_TEX_DUMP used to
// create sdmc:/<name> - a directory nobody looks in, and it failed silently if anything went wrong. Relative
// paths now land beside logs/ (sdmc:/switch/RogueLeader-NX/<name>), and the resolved path is logged so the user can
// find the files without guessing.
const char* directory() {
    static const std::string dir = [] {
        const char* d = getenv("R2_TEX_DUMP");
        if (!d || !*d) return std::string();
        std::string s = d;
#if defined(__SWITCH__)
        if (s[0] != '/' && !(s.size() > 1 && s[1] == ':')) s = std::string("sdmc:/switch/RogueLeader-NX/") + s;
#endif
        return s;
    }();
    return dir.empty() ? nullptr : dir.c_str();
}

std::string name(u32 fmt, u32 w, u32 h, bool mips, const u8* data, const u8* tlut, u32 tlutEntries) {
    const u32 bytes = levelBytes(fmt, w, h);
    char buf[96];
    const unsigned long long th = XXH64(data, bytes, 0);
    const bool palette = fmt == 8 || fmt == 9 || fmt == 10;
    if (!palette) {
        snprintf(buf, sizeof buf, "tex1_%ux%u%s_%016llx_%u", w, h, mips ? "_m" : "", th, fmt);
        return buf;
    }
    // the palette range the indices use, as Aurora (and Dolphin) hash it
    u32 lo = 0xFFFF, hi = 0;
    for (u32 i = 0; i < bytes; i++) {
        if (fmt == 8) {
            lo = std::min({lo, u32(data[i] & 15), u32(data[i] >> 4)});
            hi = std::max({hi, u32(data[i] & 15), u32(data[i] >> 4)});
        } else if (fmt == 9) {
            lo = std::min(lo, u32(data[i]));
            hi = std::max(hi, u32(data[i]));
        } else if (i % 2 == 0) {
            const u32 v = be16(data + i) & 0x3FFF;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
    unsigned long long ph = 0;
    if (tlut && lo <= hi && hi < tlutEntries) ph = XXH64(tlut + 2 * lo, 2 * (hi + 1 - lo), 0);
    snprintf(buf, sizeof buf, "tex1_%ux%u%s_%016llx_%016llx_%u", w, h, mips ? "_m" : "", th, ph, fmt);
    return buf;
}

void dump(u32 fmt, u32 w, u32 h, bool mips, const u8* data, u32 tlutFmt, const u8* tlut, u32 tlutEntries) {
    const char* dir = directory();
    if (!dir || !data) return;
    // RL-NX: GX textures are at most 1024x1024; a wider or taller bind is an uninitialised BP image
    // register, and hashing/decoding that extent reads far past guest RAM (the boot crashed at 10 s
    // once the dump was switched on). Skip it instead of trusting the register.
    if (w == 0 || h == 0 || w > 1024 || h > 1024) return;
    static std::mutex m;
    static std::unordered_set<std::string> done;
    const std::string n = name(fmt, w, h, mips, data, tlut, tlutEntries);
    std::lock_guard lock(m);
    if (done.size() > 20000 || !done.insert(n).second) return;
    std::vector<Rgba> px;
    if (!decode(fmt, w, h, data, tlutFmt, tlut, tlutEntries, px)) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    // RL-NX: only the first attempt reports the resolved path, then only real failures - a silent dump is
    // worse than no dump, because it looks like "the game binds no textures".
    static int state = 0;  // 0: first call, 1: working, 2: disabled after a failure
    const std::string file = (std::filesystem::path(dir) / (n + ".png")).string();
    if (state == 0) {
        state = 1;
        logf("tex", "texdump: writing %s to %s", n.c_str(), file.c_str());
    }
    if (ec) {
        logf("tex", "texdump: cannot create %s (%s) - dumping disabled", dir, ec.message().c_str());
        state = 2;
        return;
    }
    if (!writePng(std::filesystem::path(dir) / (n + ".png"), w, h, px)) {
        static int fails = 0;
        if (fails++ < 5) logf("tex", "texdump: cannot write %s", file.c_str());
        return;
    }
    if (state == 2) state = 1;
}

}  // namespace rt::gfx::texdump
