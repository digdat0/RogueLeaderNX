#pragma once
// R2_TEX_DUMP=<directory>: textures the game binds, as PNGs named for replacement (tex_dump.cpp)
#include "../ctx.h"

#include <string>
#include <vector>

namespace rt::gfx::texdump {
const char* directory();  // R2_TEX_DUMP, or nullptr
// Dolphin's replacement name (no extension): tex1_<w>x<h>[_m]_<hash>[_<palette hash>]_<fmt>
std::string name(u32 fmt, u32 w, u32 h, bool mips, const u8* data, const u8* tlut, u32 tlutEntries);
// level 0 to RGBA8 rows (false: a format it does not decode)
bool decodeRgba(u32 fmt, u32 w, u32 h, const u8* data, u32 tlutFmt, const u8* tlut, u32 tlutEntries, std::vector<u8>& out);
// RGBA8 rows to a PNG file
bool writePng(const std::string& path, u32 w, u32 h, const u8* rgba);
// decode level 0 and write <directory>/<name>.png (once per name)
void dump(u32 fmt, u32 w, u32 h, bool mips, const u8* data, u32 tlutFmt, const u8* tlut, u32 tlutEntries);
}  // namespace rt::gfx::texdump
