// RL-NX (Stage 10): Factor 5's .movie frames decoded by libjpeg-turbo instead of the game's own decoder.
//
// A .movie is a header plus one baseline JPEG per frame (4:2:0). The movie player (7FD8300C) parses a frame's
// markers with 0x8005481C (ctx +0x0C = the frame's first byte, +0x6C = the entropy data after SOS, +0x70/+0x72 =
// the SOF width/height) and then calls one of three frame functions on the decoder context (r3) with the Y, Cb
// and Cr planes (r4, r5, r6):
//   0x80055DA0 (movie width 512) and 0x80055E30 (640): every MCU row through a width-specialised IDCT
//     (0x80055F60, 0x80057970);
//   0x80055EC0 (any other width, r7 = the width): the general IDCT 0x80059380.
// Each writes the planes straight in GX I8 tile order (8x4 texels a 32-byte tile, tiles row by row; psq_st
// through GQR6 = u8, scale -3), Y at full size and Cb/Cr at half width and height, one plane after the other.
// The hardware profile 00:56 had the 640 IDCT at 22 ms a frame plus ~10 ms of paired-single stores (intro and
// level select ~15 fps).
//
// The frames are not quite standard JPEG: Factor 5's encoder does not byte-stuff the entropy data (a data byte
// FF is not followed by 00; frame 30 of the title movie has 29 bare FFs), and the game's decoder never looks for
// markers there. So a bare FF D9 can also appear inside a frame, and the frame's end cannot be found by scanning.
// The libjpeg source manager below therefore reads guest memory directly, from the frame start, and inserts the
// missing 00 after every FF past the entropy start. libjpeg stops reading once the last MCU is decoded, and
// jpeg_abort_decompress ends the frame without looking for EOI, so no frame length is needed. raw_data_out
// gives the Y/Cb/Cr planes as they are coded (no upsampling, no colour conversion), which are tiled into guest
// memory. Anything this does not handle exactly runs the recompiled body (<fn>_orig) instead:
// not 3-component 4:2:0, a size that is not a multiple of 16, a width other than the one the function is for, a
// plane not 32-byte aligned or not mapped (VM page not in the TLB), or any libjpeg error or warning (the frame is
// decoded to host memory first, so the guest planes are untouched then).
// libjpeg-turbo's integer IDCT is not the game's float IDCT, so pixels can differ by about 1.
// R2_MOVIE_CHECK=1 runs the game's decoder, decodes the frame here as well and logs how far they differ
// ([movie] lines; frame 30 is dumped to logs/movie_game.bin and logs/movie_tj.bin), without changing the output.
// The - menu's NATIVE MOVIE row (R2_NATIVE_MOVIE, default on) switches it off.
#include "funcs.h"
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__SWITCH__)
#include <jpeglib.h>
#endif

namespace rt {

namespace {
std::atomic<bool> s_nativeMovie{true};

#if defined(__SWITCH__)
constexpr u32 kMaxJpeg = 1u << 20;  // a frame is ~15-40 KiB; reading past this feeds EOI (libjpeg then warns)
constexpr u32 kMaxDim = 1024;

// Host pointer for guest address a (arena or a mapped VM page), or null.
inline u8* hostPtr(u32 a, bool write) {
    if (inArena(a)) return arenaPtr(a);
    if (isVm(a)) {
        u8* p = (write ? g_vmWrite : g_vmRead)[(a - kVmStart) >> 12];
        return p ? p + (a & 0xFFF) : nullptr;
    }
    return nullptr;
}
// Every page of [a, a + n) has a host pointer.
bool spanOk(u32 a, u32 n, bool write) {
    if (inArena(a) && inArena(a + n - 1)) return true;
    for (u32 p = a & ~0xFFFu; p - (a & ~0xFFFu) < n + (a & 0xFFFu); p += 0x1000)
        if (!isVm(p) || !hostPtr(p, write)) return false;
    return true;
}

// libjpeg source: the guest frame from `pos`, with a 00 inserted after every FF at or past `entropy`.
struct Src {
    jpeg_source_mgr pub;
    u32 start, pos, entropy;
    u8 buf[4096 + 2];
};
void srcInit(j_decompress_ptr) {}
boolean srcFill(j_decompress_ptr cinfo) {
    Src& s = *reinterpret_cast<Src*>(cinfo->src);
    u32 n = 0;
    while (n < 4096) {
        const u8* p = s.pos - s.start < kMaxJpeg ? hostPtr(s.pos, false) : nullptr;
        if (!p) {  // unmapped or too long: end the stream (libjpeg warns, so the frame falls back)
            if (n == 0) s.buf[n++] = 0xFF, s.buf[n++] = JPEG_EOI;
            break;
        }
        const u32 run = 0x1000u - (s.pos & 0xFFFu);  // bytes left in this page
        u32 i = 0;
        if (s.pos < s.entropy) {  // the headers: as they are
            i = std::min({run, s.entropy - s.pos, 4096u - n});
            std::memcpy(s.buf + n, p, i);
            n += i;
        } else {
            for (; i < run && n < 4096; i++) {
                const u8 b = p[i];
                s.buf[n++] = b;
                if (b == 0xFF) s.buf[n++] = 0x00;  // the stuffing the encoder left out (buf has 2 spare bytes)
            }
        }
        s.pos += i;
    }
    s.pub.next_input_byte = s.buf;
    s.pub.bytes_in_buffer = n;
    return TRUE;
}
void srcSkip(j_decompress_ptr cinfo, long n) {
    jpeg_source_mgr& s = *cinfo->src;
    while (n > long(s.bytes_in_buffer)) {
        n -= long(s.bytes_in_buffer);
        srcFill(cinfo);
    }
    s.next_input_byte += n;
    s.bytes_in_buffer -= size_t(n);
}
void srcTerm(j_decompress_ptr) {}

struct Err {
    jpeg_error_mgr pub;
    std::jmp_buf jb;
};
void errExit(j_common_ptr cinfo) { std::longjmp(reinterpret_cast<Err*>(cinfo->err)->jb, 1); }
void errOutput(j_common_ptr) {}

struct State {
    jpeg_decompress_struct cinfo;
    Err err;
    Src src;
    bool ready = false;
    std::vector<u8> planes;
    bool check = false;
    u32 frames = 0, fallbacks = 0, checked = 0;
};
State& st() {
    static State s;
    return s;
}

// Writes a w x h I8 plane (row stride `stride`) to guest address base in GX tile order.
void storeTiled(u32 base, const u8* src, u32 w, u32 h, u32 stride) {
    u32 off = 0;
    for (u32 ty = 0; ty < h; ty += 4)
        for (u32 tx = 0; tx < w; tx += 8, off += 32) {
            u8* d = hostPtr(base + off, true);  // a 32-byte tile never crosses a page (base is 32-aligned)
            const u8* s = src + ty * stride + tx;
            for (u32 r = 0; r < 4; r++) std::memcpy(d + 8 * r, s + r * stride, 8);
        }
}

// Largest |difference| and the count over 2 between a host plane and the tiled guest plane.
void diffTiled(u32 base, const u8* src, u32 w, u32 h, u32 stride, int& maxd, u32& big) {
    u32 off = 0;
    for (u32 ty = 0; ty < h; ty += 4)
        for (u32 tx = 0; tx < w; tx += 8, off += 32) {
            const u8* d = hostPtr(base + off, false);
            if (!d) continue;
            for (u32 r = 0; r < 4; r++)
                for (u32 x = 0; x < 8; x++) {
                    const int e = std::abs(int(d[8 * r + x]) - int(src[(ty + r) * stride + tx + x]));
                    if (e > maxd) maxd = e;
                    if (e > 2) big++;
                }
        }
}

// Decodes the frame from guest frame start `start` (entropy data from `entropy`) into s.planes (Y, Cb, Cr,
// linear). Returns false on any libjpeg error or warning or a frame this does not handle.
bool decodeHost(State& s, u32 start, u32 entropy, u32 wantW, u32& w, u32& h) {
    jpeg_decompress_struct& ci = s.cinfo;
    if (!s.ready) {
        ci.err = jpeg_std_error(&s.err.pub);
        s.err.pub.error_exit = errExit;
        s.err.pub.output_message = errOutput;
        if (setjmp(s.err.jb)) return false;
        jpeg_create_decompress(&ci);
        s.src.pub.init_source = srcInit;
        s.src.pub.fill_input_buffer = srcFill;
        s.src.pub.skip_input_data = srcSkip;
        s.src.pub.resync_to_restart = jpeg_resync_to_restart;
        s.src.pub.term_source = srcTerm;
        ci.src = &s.src.pub;
        s.ready = true;
    }
    s.src.start = s.src.pos = start;
    s.src.entropy = entropy;
    s.src.pub.bytes_in_buffer = 0;
    s.src.pub.next_input_byte = nullptr;
    if (setjmp(s.err.jb)) {
        jpeg_abort_decompress(&ci);
        return false;
    }
    s.err.pub.num_warnings = 0;
    if (jpeg_read_header(&ci, TRUE) != JPEG_HEADER_OK) {
        jpeg_abort_decompress(&ci);
        return false;
    }
    w = ci.image_width, h = ci.image_height;
    const jpeg_component_info* k = ci.comp_info;
    if (ci.num_components != 3 || ci.jpeg_color_space != JCS_YCbCr || k[0].h_samp_factor != 2 ||
        k[0].v_samp_factor != 2 || k[1].h_samp_factor != 1 || k[1].v_samp_factor != 1 || k[2].h_samp_factor != 1 ||
        k[2].v_samp_factor != 1 || w != wantW || h == 0 || h > kMaxDim || ((w | h) & 15)) {
        jpeg_abort_decompress(&ci);
        return false;
    }
    ci.raw_data_out = TRUE;
    ci.dct_method = JDCT_ISLOW;
    jpeg_start_decompress(&ci);
    const u32 yN = w * h, cN = yN / 4;
    s.planes.resize(yN + 2 * cN);
    u8* const Y = s.planes.data();
    u8* const U = Y + yN;
    u8* const V = U + cN;
    JSAMPROW ry[16], ru[8], rv[8];
    JSAMPARRAY rows[3] = {ry, ru, rv};
    while (ci.output_scanline < h) {
        const u32 l = ci.output_scanline;
        for (u32 i = 0; i < 16; i++) ry[i] = Y + (l + i) * w;
        for (u32 i = 0; i < 8; i++) ru[i] = U + (l / 2 + i) * (w / 2), rv[i] = V + (l / 2 + i) * (w / 2);
        if (jpeg_read_raw_data(&ci, rows, 16) != 16) break;
    }
    const bool ok = ci.output_scanline == h && s.err.pub.num_warnings == 0;
    jpeg_abort_decompress(&ci);  // not finish: that would look for EOI in data that has none where libjpeg expects
    return ok;
}

// Decodes the frame of decoder context a[0] into the planes a[1..3] (the call's r3-r6). wantW: the width the
// replaced function is specialised for. store false: compare with what the game's decoder wrote instead.
// Returns false (nothing written) when the recompiled body must run instead.
bool decode(const u32 a[4], u32 wantW, bool store) {
    State& s = st();
    const u32 ctx = a[0];
    if (!hostPtr(ctx, false) || !hostPtr(ctx + 0x70, false)) return false;
    const u32 start = ld32be(hostPtr(ctx + 0x0C, false)), entropy = ld32be(hostPtr(ctx + 0x6C, false));
    if (entropy - start >= 0x10000u) return false;
    const u32 yA = a[1], uA = a[2], vA = a[3];
    u32 w = 0, h = 0;
    if (!decodeHost(s, start, entropy, wantW, w, h)) return false;
    const u32 yN = w * h, cN = yN / 4;
    if (((yA | uA | vA) & 31) || !spanOk(yA, yN, store) || !spanOk(uA, cN, store) || !spanOk(vA, cN, store))
        return false;
    const u8* const Y = s.planes.data();
    if (store) {
        storeTiled(yA, Y, w, h, w);
        storeTiled(uA, Y + yN, w / 2, h / 2, w / 2);
        storeTiled(vA, Y + yN + cN, w / 2, h / 2, w / 2);
        if (s.frames++ == 0) logf("movie", "libjpeg-turbo frames: %ux%u 4:2:0", w, h);
        return true;
    }
    int maxd = 0;
    u32 big = 0;
    diffTiled(yA, Y, w, h, w, maxd, big);
    diffTiled(uA, Y + yN, w / 2, h / 2, w / 2, maxd, big);
    diffTiled(vA, Y + yN + cN, w / 2, h / 2, w / 2, maxd, big);
    // Frame 30 of the check run is written out for offline comparison (logs/movie_game.bin: the guest's three
    // planes as stored; logs/movie_tj.bin: libjpeg-turbo's linear planes).
    if (s.checked == 30) {
        if (FILE* f = fopen("sdmc:/switch/RL-NX/logs/movie_game.bin", "wb")) {
            const u32 at[3] = {yA, uA, vA}, n[3] = {yN, cN, cN};
            for (int i = 0; i < 3; i++)
                for (u32 o = 0; o < n[i]; o += 32)
                    if (const u8* p = hostPtr(at[i] + o, false)) fwrite(p, 1, 32, f);
            fclose(f);
        }
        if (FILE* f = fopen("sdmc:/switch/RL-NX/logs/movie_tj.bin", "wb")) {
            fwrite(s.planes.data(), 1, s.planes.size(), f);
            fclose(f);
        }
        logf("movie", "frame 30 dumped: Y %08X Cb %08X Cr %08X", yA, uA, vA);
    }
    if (s.checked < 20 || s.checked % 240 == 0)
        logf("movie", "check frame %u (%ux%u): max |diff| %d, %u of %u samples over 2", s.checked, w, h, maxd, big,
             yN + 2 * cN);
    s.checked++;
    return true;
}

// The frame functions set GQR5 (s16, the coefficient loads) and GQR6 (u8 scale -3, the pixel stores) and
// leave them set; the native does the same.
inline void setGqrs(Ctx& c) {
    c.gqr[5] = 0x00070007u;
    c.gqr[6] = 0x3D043D04u;
}

template <void (*Orig)(Ctx&)>
void frame(Ctx& c, u32 wantW) {
    State& s = st();
    static bool once = false;
    if (!once) {
        once = true;
        const char* e = getenv("R2_MOVIE_CHECK");
        s.check = e && *e == '1';
        if (s.check) logf("movie", "R2_MOVIE_CHECK: the game decodes, libjpeg-turbo is compared");
    }
    const u32 args[4] = {c.r[3], c.r[4], c.r[5], c.r[6]};  // the check decodes from what the game's decoder used
    if (s_nativeMovie.load(std::memory_order_relaxed) && !s.check) {
        if (decode(args, wantW, true)) [[likely]] {
            setGqrs(c);
            return;
        }
        if (s.fallbacks++ < 8) logf("movie", "frame not handled natively (ctx %08X, width %u): game decoder", c.r[3], wantW);
    }
    Orig(c);
    if (s.check && !decode(args, wantW, false) && s.fallbacks++ < 8)
        logf("movie", "check: libjpeg-turbo refused the frame (ctx %08X, width %u)", c.r[3], wantW);
}
#endif
}  // namespace

void setNativeMovie(bool on) { s_nativeMovie.store(on, std::memory_order_relaxed); }
bool nativeMovieOn() { return s_nativeMovie.load(std::memory_order_relaxed); }

// (ctx r3, Y r4, Cb r5, Cr r6): a 512-wide frame.
void hle_MovieFrame512(Ctx& c) {
#if defined(__SWITCH__)
    frame<f_80055DA0_orig>(c, 512);
#else
    f_80055DA0_orig(c);
#endif
}
// (ctx r3, Y r4, Cb r5, Cr r6): a 640-wide frame.
void hle_MovieFrame640(Ctx& c) {
#if defined(__SWITCH__)
    frame<f_80055E30_orig>(c, 640);
#else
    f_80055E30_orig(c);
#endif
}
// (ctx r3, Y r4, Cb r5, Cr r6, width r7): any other width.
void hle_MovieFrameAny(Ctx& c) {
#if defined(__SWITCH__)
    frame<f_80055EC0_orig>(c, c.r[7]);
#else
    f_80055EC0_orig(c);
#endif
}

}  // namespace rt
