#include "command_processor.hpp"

#include "../gfx/pipeline_cache.hpp"
#include "../gfx/recording.hpp"
#include "../gfx/resources.hpp"
#include "../internal.hpp"
#include "dolphin/gd/GDGeometry.h"
#include "dolphin/gx/GXAurora.h"
#include "gx.hpp"
#include "pipeline.hpp"
#include "readback.hpp"
#include "regs.hpp"
#include "shader_info.hpp"
#include "texture.hpp"
#include "uber.hpp"

#include <tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

namespace aurora::gx::fifo {
namespace {
constexpr Module Log{"aurora::gx::fifo"};

u16 prepare_idx_buffer(ByteBuffer& buf, GXPrimitive prim, u16 vtxStart, u16 vtxCount) noexcept {
  u16 numIndices = 0;
  // Incomplete primitives draw nothing on hardware. Keep the index list to whole triangles:
  // a strip/fan of fewer than 3 vertices also underflowed the reservation below (8 GiB for an
  // empty strip), and merged draws concatenate these lists, so stray indices would shift every
  // later triangle of the batch.
  if ((prim == GX_TRIANGLESTRIP || prim == GX_TRIANGLEFAN) && vtxCount < 3) {
    return 0;
  }
  // Quads: three vertices left over after the whole quads still make a triangle. The hardware
  // assembles a quad's first triangle as soon as its third vertex arrives (Dolphin's index
  // generator does the same); Rogue Leader draws a force field as 3-vertex GX_QUADS.
  bool quadTail = false;
  if (prim == GX_TRIANGLES) {
    vtxCount -= vtxCount % 3;
  } else if (prim == GX_QUADS) {
    quadTail = (vtxCount & 3u) == 3;
    vtxCount &= ~3u;
  }
  if (prim == GX_QUADS) {
    buf.reserve_extra(((vtxCount / 4) * 6 + (quadTail ? 3 : 0)) * sizeof(u16));

    for (u16 v = 0; v < vtxCount; v += 4) {
      u16 idx0 = vtxStart + v;
      u16 idx1 = vtxStart + v + 1;
      u16 idx2 = vtxStart + v + 2;
      u16 idx3 = vtxStart + v + 3;

      buf.append(idx0);
      buf.append(idx1);
      buf.append(idx2);
      numIndices += 3;

      buf.append(idx2);
      buf.append(idx3);
      buf.append(idx0);
      numIndices += 3;
    }
    if (quadTail) {
      const u16 idx = vtxStart + vtxCount;
      buf.append(std::array{idx, static_cast<u16>(idx + 1), static_cast<u16>(idx + 2)});
      numIndices += 3;
    }
  } else if (prim == GX_TRIANGLES) {
    buf.reserve_extra(vtxCount * sizeof(u16));
    for (u16 v = 0; v < vtxCount; ++v) {
      const u16 idx = vtxStart + v;
      buf.append(idx);
      ++numIndices;
    }
  } else if (prim == GX_TRIANGLEFAN) {
    buf.reserve_extra(((u32(vtxCount) - 3) * 3 + 3) * sizeof(u16));
    for (u16 v = 0; v < vtxCount; ++v) {
      const u16 idx = vtxStart + v;
      if (v < 3) {
        buf.append(idx);
        ++numIndices;
        continue;
      }
      buf.append(std::array{vtxStart, static_cast<u16>(idx - 1), idx});
      numIndices += 3;
    }
  } else if (prim == GX_TRIANGLESTRIP) {
    buf.reserve_extra(((static_cast<u32>(vtxCount) - 3) * 3 + 3) * sizeof(u16));
    for (u16 v = 0; v < vtxCount; ++v) {
      const u16 idx = vtxStart + v;
      if (v < 3) {
        buf.append(idx);
        ++numIndices;
        continue;
      }
      if ((v & 1) == 0) {
        buf.append(std::array{static_cast<u16>(idx - 2), static_cast<u16>(idx - 1), idx});
      } else {
        buf.append(std::array{static_cast<u16>(idx - 1), static_cast<u16>(idx - 2), idx});
      }
      numIndices += 3;
    }
  } else if (prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS) {
    buf.reserve_extra(6 * sizeof(u16));
    buf.append<u16>(0);
    buf.append<u16>(1);
    buf.append<u16>(3);
    buf.append<u16>(3);
    buf.append<u16>(2);
    buf.append<u16>(0);
    numIndices = 6;
  } else
    UNLIKELY FATAL("unsupported primitive type {}", static_cast<u32>(prim));
  return numIndices;
}

// GX FIFO opcodes - use CP_ prefix to avoid clashing with GXCommandList.h macros
constexpr u8 CP_CMD_NOP = GX_NOP;
constexpr u8 CP_CMD_LOAD_CP_REG = GX_LOAD_CP_REG;
constexpr u8 CP_CMD_LOAD_XF_REG = GX_LOAD_XF_REG;
constexpr u8 CP_CMD_LOAD_INDX_A = GX_LOAD_INDX_A;
constexpr u8 CP_CMD_LOAD_INDX_B = GX_LOAD_INDX_B;
constexpr u8 CP_CMD_LOAD_INDX_C = GX_LOAD_INDX_C;
constexpr u8 CP_CMD_LOAD_INDX_D = GX_LOAD_INDX_D;
constexpr u8 CP_CMD_CALL_DL = GX_CMD_CALL_DL;
constexpr u8 CP_CMD_INVAL_VTX = GX_CMD_INVL_VC;
constexpr u8 CP_CMD_LOAD_BP_REG = GX_LOAD_BP_REG & GX_OPCODE_MASK;

// Primitive type mask
constexpr u8 CP_OPCODE_MASK = GX_OPCODE_MASK;
constexpr u8 CP_VAT_MASK = GX_VAT_MASK;

struct FogRangeLutKey {
  std::array<u16, 10> rangeK;
  f32 rangeCenter;
  f32 renderWidth;
  u32 targetWidth;

  bool operator==(const FogRangeLutKey&) const = default;
};

struct FogRangeLutEntry {
  FogRangeLutKey key;
  std::vector<f32> factors;
};

constexpr size_t MaxFogRangeLuts = 32;
std::vector<FogRangeLutEntry> sFogRangeLuts;

struct DrawCache {
  uint64_t targetLayoutKey = 0;
  PipelineConfig config{};
  ShaderInfo shaderInfo{};
  gfx::PipelineRef pipelineRef{};
  gfx::PipelineRef uberRef{};    // the ubershader pipeline for the same fixed-function state
  bool specializedReady = false; // pipelineRef is built (it stays so)
  bool uniformUber = false;      // uniformRange holds the ubershader's block
  GXBindGroups bindGroups{};
  uint64_t bindGeneration = 0;
  GXVtxFmt fmt = GX_MAX_VTXFMT;
  u8 lineMode = 0;
  bool hasPipeline = false;
  gfx::Range uniformRange{};
  gfx::Range fogRange{};
  FogRangeLutKey fogRangeKey{};
  bool hasFogRange = false;
  GXVtxFmt lastDrawFmt = GX_MAX_VTXFMT;
  u32 zFreezePlaneGen = ~0u; // plane in the current uniform
  u32 nbtCacheGen = ~0u;     // GXState::nbtCache in the current uniform
};
DrawCache sDrawCache;

FogRangeLutKey fog_range_lut_key() noexcept {
  const auto& state = g_gxState.fog;
  const f32 logicalWidth = std::max(g_gxState.logicalViewport.width, 1.f);
  const f32 renderWidth = std::max(g_gxState.renderViewport.width, 1.f);
  return {
      .rangeK = state.rangeK,
      .rangeCenter = ((static_cast<f32>(state.rangeCenter) - g_gxState.logicalViewport.left) / logicalWidth) * 2.f -
                     1.f + (g_gxState.renderViewport.left / renderWidth) * 2.f,
      .renderWidth = renderWidth,
      .targetWidth = gfx::get_render_target_size().x,
  };
}

std::vector<f32> build_fog_range_lut(const FogRangeLutKey& key) {
  std::array<f32, 10> rangeK;
  for (u32 i = 0; i < rangeK.size(); ++i) {
    const u32 source = (i & ~1u) | (1u - (i & 1u));
    rangeK[i] = static_cast<f32>(key.rangeK[source]) / 64.f;
  }

  std::vector<f32> lut(key.targetWidth);
  for (u32 x = 0; x < key.targetWidth; ++x) {
    const f32 screenX = ((static_cast<f32>(x) + 0.5f) / key.renderWidth) * 2.f - 1.f;
    const f32 offset = screenX - key.rangeCenter;
    const f32 rangeIndex = std::clamp(9.f - std::abs(offset) * 9.f, 0.f, 9.f);
    const u32 lower = static_cast<u32>(rangeIndex);
    const u32 upper = std::min(lower + 1, 9u);
    const f32 fraction = rangeIndex - static_cast<f32>(lower);
    const f32 k = std::max(rangeK[lower] * (1.f - fraction) + rangeK[upper] * fraction, 0.000001f);
    lut[x] = std::sqrt(offset * offset + k * k) / k;
  }
  return lut;
}

const std::vector<f32>& resolve_fog_range_lut(const FogRangeLutKey& key) {
  for (const auto& entry : sFogRangeLuts) {
    if (entry.key == key) {
      return entry.factors;
    }
  }
  if (sFogRangeLuts.size() == MaxFogRangeLuts) {
    sFogRangeLuts.erase(sFogRangeLuts.begin());
  }
  sFogRangeLuts.emplace_back(FogRangeLutEntry{key, build_fog_range_lut(key)});
  return sFogRangeLuts.back().factors;
}

gfx::Range push_fog_range_lut(const FogRangeLutKey& key) {
  const auto& lut = resolve_fog_range_lut(key);
  return gfx::push_storage(reinterpret_cast<const u8*>(lut.data()), lut.size() * sizeof(f32));
}

u8 line_mode_for_prim(GXPrimitive prim) noexcept {
  switch (prim) {
  case GX_LINES:
    return 1;
  case GX_LINESTRIP:
    return 2;
  case GX_POINTS:
    return 3;
  default:
    return 0;
  }
}

// zfreeze reference plane. Hardware gives primitives drawn with GenMode zfreeze the depth slope of
// the last triangle rasterized without it (games send one for the purpose, often with
// GX_CULL_ALL). Like Dolphin, take the last three vertices of each triangle draw made with
// zfreeze off, transform them as the vertex shader does, and keep their plane in render target
// pixels.
float read_component(const u8* p, GXCompType type, u8 frac, bool le) noexcept {
  const auto rd16 = [&](const u8* q) -> u16 { return le ? u16(q[0] | (q[1] << 8)) : u16((q[0] << 8) | q[1]); };
  const float scale = 1.f / static_cast<float>(1u << frac);
  switch (type) {
  case GX_U8:
    return static_cast<float>(p[0]) * scale;
  case GX_S8:
    return static_cast<float>(static_cast<s8>(p[0])) * scale;
  case GX_U16:
    return static_cast<float>(rd16(p)) * scale;
  case GX_S16:
    return static_cast<float>(static_cast<s16>(rd16(p))) * scale;
  case GX_F32: {
    const u32 v = le ? u32(p[0] | (p[1] << 8) | (p[2] << 16) | (u32(p[3]) << 24))
                     : u32((u32(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
    return std::bit_cast<float>(v);
  }
  default:
    return 0.f;
  }
}

// Where a vertex format keeps its position (and per-vertex matrix index), from the current
// vertex descriptor and attribute format.
struct PosLayout {
  bool valid = false;
  u32 posOffset = 0;
  GXAttrType posType = GX_NONE;
  GXCompType compType = GX_F32;
  u8 cnt = 3, size = 4, frac = 0;
  int mtxIdxOffset = -1; // GX_VA_PNMTXIDX in the vertex, or -1: the CP's current matrix
  const u8* array = nullptr;
  u32 arraySize = 0, arrayStride = 0;
  bool arrayLe = false;
};

PosLayout pos_layout(GXVtxFmt fmt) noexcept {
  const auto& state = g_gxState;
  const auto& vtxFmt = state.vtxFmts[fmt];
  PosLayout l;
  u32 offset = 0;
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_POS; ++i) {
    const auto attr = static_cast<GXAttr>(i);
    const auto type = state.vtxDesc[i];
    const auto& attrFmt = vtxFmt.attrs[i];
    if (type == GX_NONE) {
      continue;
    }
    if (attr == GX_VA_POS) {
      l.posOffset = offset;
      l.posType = type;
      l.compType = attrFmt.type;
      l.cnt = comp_cnt_count(attr, attrFmt.cnt);
      l.size = comp_type_size(attr, attrFmt.type);
      l.frac = attrFmt.frac;
      const auto& array = state.arrays[GX_VA_POS];
      l.array = static_cast<const u8*>(array.data);
      l.arraySize = array.size;
      l.arrayStride = array.stride;
      l.arrayLe = array.le;
      l.valid = true;
      return l;
    }
    if (attr == GX_VA_PNMTXIDX) {
      l.mtxIdxOffset = static_cast<int>(offset);
    }
    if (type == GX_DIRECT) {
      offset += comp_type_size(attr, attrFmt.type) * comp_cnt_count(attr, attrFmt.cnt);
    } else {
      offset += type == GX_INDEX8 ? 1 : 2;
    }
  }
  return l;
}

bool layout_position(const PosLayout& l, const u8* vtx, Vec4<float>& out) noexcept {
  const u8* src = vtx + l.posOffset;
  bool le = false;
  if (l.posType == GX_INDEX8 || l.posType == GX_INDEX16) {
    const u32 idx = l.posType == GX_INDEX8 ? src[0] : (u32(src[0]) << 8 | src[1]);
    const u32 at = idx * l.arrayStride;
    if (l.array == nullptr || at > l.arraySize || u32(l.size) * l.cnt > l.arraySize - at) {
      return false;
    }
    src = l.array + at;
    le = l.arrayLe;
  }
  out = {read_component(src, l.compType, l.frac, le), read_component(src + l.size, l.compType, l.frac, le),
         l.cnt == 3 ? read_component(src + 2 * l.size, l.compType, l.frac, le) : 0.f, 1.f};
  return true;
}

// XF matrix row / 3 -> the position matrix the vertex shader uses (postex_mtx)
const Mat3x4<float>& position_matrix(u32 mtx) noexcept {
  static const Mat3x4<float> identity{{1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}};
  if (mtx < MaxPnMtx) {
    return g_gxState.pnMtx[mtx].pos;
  }
  if (mtx < MaxPnMtx + MaxTexMtx) {
    return g_gxState.texMtxs[mtx - MaxPnMtx];
  }
  return identity; // GX_IDENTITY
}

u32 vertex_matrix(const PosLayout& l, const u8* vtx) noexcept {
  return l.mtxIdxOffset >= 0 ? vtx[l.mtxIdxOffset] / 3u : g_gxState.currentPnMtx;
}

Vec4<float> transform(const Mat3x4<float>& m, const Vec4<float>& p) noexcept {
  const auto dot4 = [](const Vec4<float>& a, const Vec4<float>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  };
  return {dot4(p, m.m0), dot4(p, m.m1), dot4(p, m.m2), 1.f};
}

// Model-view position of one vertex of raw vertex data, as the vertex shader computes it.
bool vertex_mv_pos(const PosLayout& l, const u8* vtx, Vec4<float>& out) noexcept {
  Vec4<float> pos;
  if (!l.valid || !layout_position(l, vtx, pos)) {
    return false;
  }
  out = transform(position_matrix(vertex_matrix(l, vtx)), pos);
  return true;
}

// Widescreen (AuroraSetDisplayAspect): how much wider the render target is than the logical
// frame buffer it stands for.
float widescreen_factor() noexcept {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return 1.f;
  }
  // (every draw asks: recomputed only when the target size changes)
  static Vec2<uint32_t> lastTarget{0, 0};
  static float lastK = 1.f;
  const auto target = gfx::get_render_target_size();
  if (target.x == lastTarget.x && target.y == lastTarget.y) {
    return lastK;
  }
  lastTarget = target;
  const auto [logicalWidth, logicalHeight] = logical_fb_size();
  if (logicalWidth == 0 || logicalHeight == 0 || target.x == 0 || target.y == 0) {
    return lastK = 1.f;
  }
  const float k = (static_cast<float>(target.x) / static_cast<float>(target.y)) /
                  (static_cast<float>(logicalWidth) / static_cast<float>(logicalHeight));
  return lastK = std::abs(k - 1.f) < 0.002f ? 1.f : k;
}

// Horizontal projection scale for a draw on a widened target: 3D gets the wider field of view;
// 2D keeps its logical aspect, centered (HUD, menus, text), unless the draw spans the whole
// viewport width (backgrounds, fades, letterbox bars, render-to-texture passes), which stretch.
// Letterboxed frames with letterboxFill: how much the presentation stretches them vertically
// (the logical frame buffer's height over the XFB's lines), 1 otherwise. Only clearly letterboxed
// frames (cinematics at 304 lines, not gameplay's 450).
float letterbox_factor() noexcept {
  if (!g_gxState.letterboxFill || g_gxState.xfbHeight == 0) {
    return 1.f;
  }
  const float logicalHeight = static_cast<float>(logical_fb_size().y);
  const float lines = static_cast<float>(g_gxState.xfbHeight);
  return lines < logicalHeight * 0.85f ? logicalHeight / lines : 1.f;
}

struct ProjScale {
  float x = 1.f, y = 1.f, xOffset = 1.f, xAdd = 0.f, yAdd = 0.f;
};

// The scales the scene's (centered) perspective projection got last: screen-position sprites
// follow them.
ProjScale sScenePersp;

// A screen-position sprite: one additive quad in normalized device coordinates (identity
// orthographic projection), e.g. the engine glows (Rogue Leader's red halos around ships'
// engines) and lens flares a game places where its 3D projection puts a point. Its position
// comes from the scene's projection, its size is laid out for 4:3. (Additive only: 2D pieces
// laid out in 4:3 keep the usual treatment.)
bool is_screen_sprite(GXPrimitive prim, u32 vtxCount) noexcept {
  const auto& s = g_gxState;
  const auto& p = s.proj;
  return s.projType == GX_ORTHOGRAPHIC && prim == GX_QUADS && vtxCount == 4 && p.m0[0] == 1.f && p.m1[1] == 1.f &&
         p.m0[3] == 0.f && p.m1[3] == 0.f && s.blendMode == GX_BM_BLEND && s.blendFacDst == GX_BL_ONE;
}

// HUD anchoring: a piece whose center is this far out (NDC) and that stays on one side.
constexpr float HudAnchorCenter = 0.45f;

// Projection scales for a draw on a widened target (and in a letterboxed frame stretched to fill
// it). 3D keeps the framing the game had along the axis that fits and opens up the other: wider
// field of view on a wider target, taller for cinematics that are wider than the display.
ProjScale perspective_scale(float k, float v) noexcept {
  // A projection the game already made for the wider target (its own camera aspect widened,
  // so that it also culls for it) stays as it is. (Exactly the logical aspect times the
  // widening: other aspects, e.g. letterboxed cutscene cameras, are the game's own.)
  const float p0 = g_gxState.proj.m0[0], p2 = g_gxState.proj.m1[1];
  const auto [logicalWidth, logicalHeight] = logical_fb_size();
  const float logicalAspect = static_cast<float>(logicalWidth) / static_cast<float>(std::max(logicalHeight, 1u));
  // Its off-center shift (e.g. the HUD radar's camera, aimed at a corner) still follows the
  // 4:3 layout the rest of the HUD keeps.
  // HUD anchoring: a camera aimed off-center (its axis at x_ndc = -p1, the 4:3 layout) at a
  // side moves with the 2D HUD it belongs to.
  float xAdd = 0.f;
  if (g_gxState.hudAnchoring && p0 != 0.f) {
    const float center = -g_gxState.proj.m0[2];
    if (center > HudAnchorCenter) {
      xAdd = 1.f - 1.f / k;
    } else if (center < -HudAnchorCenter) {
      xAdd = 1.f / k - 1.f;
    }
  }
  // A letterboxed frame's own camera: the logical aspect times the letterbox stretch (4:3 *
  // 480/304 = 2.105 for 304-line cinematics). It is the game's, also on a display of about that
  // aspect: a 2.12:1 window passed the 1% test below and such frames were squeezed by 1 / v
  // (cards and movies stopped short of the top and bottom).
  const bool letterboxCamera =
      v > 1.f && p0 != 0.f && std::abs(std::abs(p2 / p0) / (logicalAspect * v) - 1.f) < 0.002f;
  if (!letterboxCamera && p0 != 0.f && std::abs(std::abs(p2 / p0) / (logicalAspect * k) - 1.f) < 0.01f) {
    return {1.f, 1.f / v, 1.f / k, xAdd};
  }
  // others the game widened itself (AuroraSetWidenedAspects)
  for (u32 i = 0; !letterboxCamera && p0 != 0.f && i < g_gxState.widenedAspectCount; ++i) {
    if (std::abs(std::abs(p2 / p0) / g_gxState.widenedAspects[i] - 1.f) < 0.002f) {
      return {1.f, 1.f / v, 1.f / k, xAdd};
    }
  }
  // shown k times wider and v times taller than the game drew it for
  const float r = k / v;
  return r >= 1.f ? ProjScale{1.f / r, 1.f, 1.f, xAdd} : ProjScale{1.f, r, 1.f, xAdd};
}

// The picture of a letterboxed frame, drawn for a copy: cinematics render a 640x264 EFB in the
// anti-aliased pixel format, copy it to a texture and stretch that into the short XFB (Rogue
// Leader's crawl, cutscenes and credits). 2D drawn there is part of the picture (the credits'
// names), so it opens with the picture as the picture's 3D does (perspective_scale: k wider and
// v taller than it was drawn for) instead of keeping its 4:3 size around the middle of its
// viewport - a 640x480 one, whose middle row (240) lies below the 264-line picture's (132): the
// credits shrank by 304/480 toward the bottom, and their lines vanished a third of the way down
// the screen (the game drops a line as it leaves the picture's top).
constexpr float AaEfbHeight = 264.f;

ProjScale letterbox_picture_scale(float k, float v) noexcept {
  const float r = k / v;
  if (r >= 1.f) {
    return {1.f / r, 1.f};
  }
  // taller than drawn for: about the picture's middle row
  const auto& vp = g_gxState.logicalViewport;
  const float cy = vp.height > 0.f ? 1.f - 2.f * (AaEfbHeight * 0.5f - vp.top) / vp.height : 0.f;
  return {1.f, r, 1.f, 0.f, cy * (1.f - r)};
}

// 2D keeps its logical shape, centered (HUD, menus, text), unless the draw spans the whole
// viewport width (backgrounds, fades, letterbox bars, render-to-texture passes), which stretches.
// A screen-position sprite keeps its 4:3 shape around its own center, and its center goes where
// the scene's projection was moved: x' = x / k + cx (sx - 1 / k), likewise y with v.
ProjScale ortho_scale(GXVtxFmt fmt, const u8* verts, u32 vtxCount, u32 vtxSize, GXPrimitive prim, float k,
                      float v) noexcept {
  if (g_gxState.screenSpace2D) {
    return {};
  }
  const auto& proj = g_gxState.proj;
  const PosLayout layout = pos_layout(fmt);
  float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
  for (u32 i = 0; i < vtxCount; ++i) {
    Vec4<float> mv;
    if (!vertex_mv_pos(layout, verts + i * vtxSize, mv)) {
      return {1.f / k, 1.f / v};
    }
    const float w = proj.m3[0] * mv[0] + proj.m3[1] * mv[1] + proj.m3[2] * mv[2] + proj.m3[3];
    const float x = proj.m0[0] * mv[0] + proj.m0[1] * mv[1] + proj.m0[2] * mv[2] + proj.m0[3];
    const float y = proj.m1[0] * mv[0] + proj.m1[1] * mv[1] + proj.m1[2] * mv[2] + proj.m1[3];
    const float ndc = w != 0.f ? x / w : x;
    const float ndcY = w != 0.f ? y / w : y;
    minX = std::min(minX, ndc);
    maxX = std::max(maxX, ndc);
    minY = std::min(minY, ndcY);
    maxY = std::max(maxY, ndcY);
  }
  if (minX <= -0.95f && maxX >= 0.95f) {
    return {};
  }
  if (v > 1.f && g_gxState.pixelFmt == GX_PF_RGB565_Z16) {
    return letterbox_picture_scale(k, v);
  }
  const float center = (minX + maxX) * 0.5f;
  if (is_screen_sprite(prim, vtxCount)) {
    const float cy = (minY + maxY) * 0.5f;
    return {1.f / k, 1.f / v, 1.f, center * (sScenePersp.x - 1.f / k), cy * (sScenePersp.y - 1.f / v)};
  }
  // HUD anchoring: x_ndc' = (x_ndc +- 1) / k -+ 1 keeps the piece's distance to its edge
  if (g_gxState.hudAnchoring && minX >= 0.f && center > HudAnchorCenter) {
    return {1.f / k, 1.f / v, 1.f, 1.f - 1.f / k};
  }
  if (g_gxState.hudAnchoring && maxX <= 0.f && center < -HudAnchorCenter) {
    return {1.f / k, 1.f / v, 1.f, 1.f / k - 1.f};
  }
  return {1.f / k, 1.f / v};
}

// A render-to-texture pass: drawn into a viewport narrower than the frame that is the source
// rectangle of a texture copy (Rogue Leader's shadow maps: the craft's and the AT-ATs'
// silhouettes, orthographic, in 128x128 / 256x256 viewports at the frame's top left, copied to
// textures every frame). On a widened target its logical viewport stretches k times wider and
// the copy narrows it back, so it must be drawn as the game drew it: as 2D it was kept at its 4:3
// shape (narrower by 1 / k in the texture: shadows too narrow, toward the middle), as 3D it got
// a k times wider view.
bool render_to_texture_viewport() noexcept {
  const auto& vp = g_gxState.logicalViewport;
  if (vp.width >= static_cast<float>(logical_fb_size().x) * 0.9f) {
    return false; // the frame itself (copies of the whole EFB: refraction, the targeting computer)
  }
  const bool rtt = std::any_of(g_gxState.rttRects.begin(), g_gxState.rttRects.end(), [&](const gfx::ClipRect& r) {
    return r.width > 0 && std::abs(vp.left - static_cast<float>(r.x)) <= 2.f &&
           std::abs(vp.top - static_cast<float>(r.y)) <= 2.f && std::abs(vp.width - static_cast<float>(r.width)) <= 2.f &&
           std::abs(vp.height - static_cast<float>(r.height)) <= 2.f;
  });
  if (rtt) { // logged once per viewport (the first few)
    static std::array<std::array<float, 4>, 8> logged{};
    static u32 count = 0;
    const std::array<float, 4> key{vp.left, vp.top, vp.width, vp.height};
    if (count < logged.size() && std::find(logged.begin(), logged.begin() + count, key) == logged.begin() + count) {
      logged[count++] = key;
      Log.info("render-to-texture viewport {}x{} at ({}, {}): drawn without widescreen scaling", vp.width, vp.height,
               vp.left, vp.top);
    }
  }
  return rtt;
}

ProjScale widescreen_scale(GXVtxFmt fmt, const u8* verts, u32 vtxCount, u32 vtxSize, GXPrimitive prim) noexcept {
  const float k = widescreen_factor();
  const float v = k == 1.f ? 1.f : letterbox_factor();
  if (k == 1.f && v == 1.f) {
    sScenePersp = {};
    return {};
  }
  if (render_to_texture_viewport()) {
    return {};
  }
  if (g_gxState.projType != GX_ORTHOGRAPHIC) {
    const ProjScale ps = perspective_scale(k, v);
    if (std::abs(g_gxState.proj.m0[2]) < 0.01f && std::abs(g_gxState.proj.m1[2]) < 0.01f) {
      sScenePersp = ps;
    }
    return ps;
  }
  return ortho_scale(fmt, verts, vtxCount, vtxSize, prim, k, v);
}

// GX_AURORA_SET_FILL_SCREEN: the draw's extent on the target (NDC with the scales in ps, less the
// shift its projection carries for part of the frame) and, if it covers most of the target but
// an edge stops inside it, scales on top of ps that move that edge out: along each axis
// [a, b] -> [min(a, -1), max(b, 1)].
ProjScale fill_screen_scale(ProjScale ps, GXVtxFmt fmt, const u8* verts, u32 vtxCount, u32 vtxSize) noexcept {
  const auto& s = g_gxState;
  // the x and y rows as apply_widescreen_scale leaves them
  const Vec4<float> m0 = s.proj.m0 * Vec4{ps.x, ps.x, ps.x * ps.xOffset, ps.x} +
                         s.proj.m3 * Vec4{ps.xAdd, ps.xAdd, ps.xAdd, ps.xAdd};
  const Vec4<float> m1 = s.proj.m1 * Vec4{ps.y, ps.y, ps.y, ps.y} + s.proj.m3 * Vec4{ps.yAdd, ps.yAdd, ps.yAdd, ps.yAdd};
  // the carried shift as the scales moved it (perspective: it sits in the z column)
  const float shiftX = s.fillShiftX * ps.x * (s.projType == GX_ORTHOGRAPHIC ? 1.f : ps.xOffset);
  const float shiftY = s.fillShiftY * ps.y;
  const PosLayout layout = pos_layout(fmt);
  float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
  for (u32 i = 0; i < vtxCount; ++i) {
    Vec4<float> mv;
    if (!vertex_mv_pos(layout, verts + i * vtxSize, mv)) {
      return ps;
    }
    const auto dot = [&mv](const Vec4<float>& r) { return r[0] * mv[0] + r[1] * mv[1] + r[2] * mv[2] + r[3] * mv[3]; };
    const float w = dot(s.proj.m3);
    if (w <= 1e-6f) {
      return ps; // reaches behind the camera
    }
    const float x = dot(m0) / w - shiftX, y = dot(m1) / w - shiftY;
    minX = std::min(minX, x);
    maxX = std::max(maxX, x);
    minY = std::min(minY, y);
    maxY = std::max(maxY, y);
  }
  if (maxX - minX < 1.f || maxY - minY < 1.f || maxX <= -1.f || minX >= 1.f || maxY <= -1.f || minY >= 1.f) {
    return ps; // not most of the target
  }
  // ndc' = f * (ndc - shift) + t + shift, on top of the scales so far
  const auto fit = [](float a, float b, float shift, float& scale, float& add) {
    const float a2 = std::min(a, -1.f), b2 = std::max(b, 1.f);
    if (a2 == a && b2 == b) {
      return;
    }
    const float f = (b2 - a2) / (b - a), t = a2 - f * a;
    scale *= f;
    add = add * f + t + shift - f * shift;
  };
  fit(minX, maxX, shiftX, ps.x, ps.xAdd);
  fit(minY, maxY, shiftY, ps.y, ps.yAdd);
  return ps;
}

// The zfreeze reference: what the plane needs of the last triangle drawn without zfreeze, taken
// at that draw (state may change before a zfreeze draw comes, e.g. the matrix index) and turned
// into a plane only when one does - most frames have none.
struct ZFreezeRef {
  bool pending = false;
  PosLayout layout;
  u32 vtxSize = 0;
  std::array<u8, 3 * 256> verts{};
  Mat3x4<float> mtx[3];
  Mat4x4<float> proj; // as the vertex shader gets it
  double depthBase = 0.0, depthRange = 1.0;
  gfx::Viewport vp;
};
ZFreezeRef sZRef;
// RL-NX (R2_ZFREEZE_LOG=1): how many draws have taken their depth from the plane, and how many of those were
// logged. File-statics so the two Log.info lines below stay one per draw and not one per frame.
static unsigned long s_zfreezeDraws = 0, s_zfreezeLogged = 0;

Mat4x4<float> shader_projection() noexcept {
  // the projection as fill_uniform hands it to the vertex shader
  auto proj = g_gxState.proj;
  if constexpr (UseReversedZ) {
    proj.m2 = proj.m2 * Vec4{-1.f, -1.f, -1.f, -1.f};
  } else {
    proj.m2 = proj.m2 + proj.m3;
  }
  apply_widescreen_scale(proj);
  return proj;
}

void note_zfreeze_reference(GXVtxFmt fmt, const u8* lastVerts, u32 vtxSize) noexcept {
  auto& z = sZRef;
  z.layout = pos_layout(fmt);
  if (!z.layout.valid || 3 * vtxSize > z.verts.size()) {
    z.pending = false;
    return;
  }
  std::memcpy(z.verts.data(), lastVerts, 3 * vtxSize);
  z.vtxSize = vtxSize;
  for (u32 i = 0; i < 3; ++i) {
    z.mtx[i] = position_matrix(vertex_matrix(z.layout, z.verts.data() + i * vtxSize));
  }
  z.proj = shader_projection();
  const auto& lvp = g_gxState.logicalViewport;
  z.depthBase = UseReversedZ ? 1.0 - lvp.zfar : lvp.znear;
  z.depthRange = lvp.zfar - lvp.znear;
  z.vp = g_gxState.renderViewport;
  z.pending = true;
}

// The binormal / tangent cache (GXState::nbtCache): a draw whose format has NBT normals leaves its
// last vertex's binormal and tangent behind, as the vertex unit holds them; draws without them
// that need them (emboss texgens, texgens from the binormal / tangent) read those.
void note_nbt(GXVtxFmt fmt, const u8* lastVert) noexcept {
  const auto& state = g_gxState;
  const auto type = state.vtxDesc[GX_VA_NRM];
  const auto& nrmFmt = state.vtxFmts[fmt].attrs[GX_VA_NRM];
  if (type == GX_NONE || nrmFmt.cnt == GX_NRM_XYZ) {
    return;
  }
  u32 offset = 0;
  for (int i = GX_VA_PNMTXIDX; i < GX_VA_NRM; ++i) {
    const auto attr = static_cast<GXAttr>(i);
    const auto t = state.vtxDesc[i];
    if (t == GX_DIRECT) {
      const auto& f = state.vtxFmts[fmt].attrs[i];
      offset += comp_type_size(attr, f.type) * comp_cnt_count(attr, f.cnt);
    } else if (t == GX_INDEX8 || t == GX_INDEX16) {
      offset += t == GX_INDEX8 ? 1 : 2;
    }
  }
  const u8* src = lastVert + offset;
  const u32 size = comp_type_size(GX_VA_NRM, nrmFmt.type);
  const u8* b = nullptr;
  const u8* t = nullptr;
  bool le = false;
  if (type == GX_DIRECT) {
    b = src + 3 * size;
    t = src + 6 * size;
  } else {
    const auto& array = state.arrays[GX_VA_NRM];
    const u8* data = static_cast<const u8*>(array.data);
    const u32 idxSize = type == GX_INDEX8 ? 1 : 2;
    const auto index = [&](u32 n) { return idxSize == 1 ? u32(src[n]) : (u32(src[2 * n]) << 8 | src[2 * n + 1]); };
    // NBT3: three indices (normal, binormal, tangent) of 3-component entries; NBT: one index of a
    // 9-component entry
    const u32 bAt = nrmFmt.cnt == GX_NRM_NBT3 ? index(1) * array.stride : index(0) * array.stride + 3 * size;
    const u32 tAt = nrmFmt.cnt == GX_NRM_NBT3 ? index(2) * array.stride : index(0) * array.stride + 6 * size;
    if (data == nullptr || bAt > array.size || tAt > array.size || 3 * size > array.size - bAt ||
        3 * size > array.size - tAt) {
      return;
    }
    b = data + bAt;
    t = data + tAt;
    le = array.le;
  }
  const auto read3 = [&](const u8* p) {
    return Vec4<float>{read_component(p, nrmFmt.type, nrmFmt.frac, le),
                       read_component(p + size, nrmFmt.type, nrmFmt.frac, le),
                       read_component(p + 2 * size, nrmFmt.type, nrmFmt.frac, le), 0.f};
  };
  const Vec4<float> binormal = read3(b), tangent = read3(t);
  auto& cache = g_gxState.nbtCache;
  if (std::memcmp(&cache[0], &binormal, sizeof binormal) != 0 || std::memcmp(&cache[1], &tangent, sizeof tangent) != 0) {
    cache[0] = binormal;
    cache[1] = tangent;
    ++g_gxState.nbtCacheGen;
  }
}

// The draw's shader reads GXState::nbtCache (ShaderInfo::nbtCache, a superset: any enabled texgen)
bool reads_nbt_cache(GXVtxFmt fmt) noexcept {
  const auto& state = g_gxState;
  if (state.vtxDesc[GX_VA_NRM] != GX_NONE && state.vtxFmts[fmt].attrs[GX_VA_NRM].cnt != GX_NRM_XYZ) {
    return false;
  }
  for (u32 i = 0; i < state.numTexGens && i < MaxTexCoord; ++i) {
    const auto& tcg = state.tcgs[i];
    if ((tcg.type >= GX_TG_BUMP0 && tcg.type <= GX_TG_BUMP7) || tcg.src == GX_TG_BINRM || tcg.src == GX_TG_TANGENT) {
      return true;
    }
  }
  return false;
}

// RL-NX (R2_ZFREEZE_GUARD=1): a zfreeze draw writes the reference plane's depth into the whole primitive, so a
// plane that is not a depth function is not a small error - it is a whole-screen constant. Run 4
// (11252026-1025) logged the planes and they are not depths at all: `-3185723047936 -144038467919679361385152643072
// 546779655190486643832283874000371712`, and `-555531554548543596752385909702262784 -inf inf` on the next one.
// Those clamp to 1.0 (or to NaN) over the whole viewport, which is the far end of this port's reversed depth: every
// depth-tested draw behind it is rejected while the undepth-tested stars, sprites and HUD still paint. The guard
// rejects a fit that is not a plane and writes the flat far depth a backdrop should write instead (0.0), which
// cannot reject anything. Default off: it is a candidate fix, not proven on hardware.
namespace {
bool zfreeze_plane_sane(double a, double b, double c) noexcept {
  // A depth slope is per pixel in a 640x480-ish target: |a| and |b| above 1 mean depth 1.0 within one pixel, and
  // |c| above 2 is outside the depth range at the origin. Anything beyond that came from a near-zero determinant.
  return std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::fabs(a) <= 1.0 && std::fabs(b) <= 1.0 &&
         std::fabs(c) <= 2.0;
}
void zfreeze_plane_flat(const char* why) noexcept {
  // RL-NX: on by default since the SD layout change (2026-11-26); R2_ZFREEZE_GUARD=0 turns it off (diagnostic only).
  static const bool guard = [] {
    const char* e = getenv("R2_ZFREEZE_GUARD");
    return !(e != nullptr && *e == '0');
  }();
  if (!guard) {
    return; // keep the previous plane, as this code always did
  }
  static unsigned replaced = 0;
  if (++replaced <= 8 || replaced % 2000 == 0) {
    Log.info("zfreeze plane rejected: {} ({} so far); flat far depth written instead", why, replaced);
  }
  g_gxState.zFreezePlane = {0.f, 0.f, 0.f, 0.f};
  ++g_gxState.zFreezePlaneGen;
}
} // namespace

void update_zfreeze_plane() noexcept {
  auto& z = sZRef;
  if (!z.pending) {
    return;
  }
  z.pending = false;
  double sx[3], sy[3], sz[3];
  for (u32 i = 0; i < 3; ++i) {
    Vec4<float> pos;
    if (!layout_position(z.layout, z.verts.data() + i * z.vtxSize, pos)) {
      zfreeze_plane_flat("vertex unreadable");
      return;
    }
    const Vec4<float> mv = transform(z.mtx[i], pos);
    double clip[4];
    for (int r = 0; r < 4; ++r) {
      const auto& row = z.proj[r];
      clip[r] = double(row[0]) * mv[0] + double(row[1]) * mv[1] + double(row[2]) * mv[2] + double(row[3]);
    }
    if (clip[2] >= 0.0 && clip[2] <= clip[3]) {
      clip[2] = std::clamp(clip[3] * z.depthBase + clip[2] * z.depthRange, 0.0, clip[3]);
    }
    if (clip[3] == 0.0) {
      zfreeze_plane_flat("w == 0");
      return;
    }
    const double invW = 1.0 / clip[3];
    sx[i] = z.vp.left + (clip[0] * invW + 1.0) * 0.5 * z.vp.width;
    sy[i] = z.vp.top + (1.0 - clip[1] * invW) * 0.5 * z.vp.height;
    sz[i] = clip[2] * invW;
    // RL-NX: a w this small is what turned the plane into 1e26 - the reference triangle was all but at the eye.
    if (!std::isfinite(sx[i]) || !std::isfinite(sy[i]) || !std::isfinite(sz[i]) || std::fabs(invW) > 1e6) {
      zfreeze_plane_flat("vertex off the screen or w near 0");
      return;
    }
  }
  // depth = a * x + b * y + c through the three screen-space vertices
  const double dx1 = sx[1] - sx[0], dy1 = sy[1] - sy[0], dz1 = sz[1] - sz[0];
  const double dx2 = sx[2] - sx[0], dy2 = sy[2] - sy[0], dz2 = sz[2] - sz[0];
  const double det = dx1 * dy2 - dx2 * dy1;
  if (det == 0.0 || !std::isfinite(det)) {
    zfreeze_plane_flat("degenerate reference triangle");
    return;
  }
  const double a = (dz1 * dy2 - dz2 * dy1) / det;
  const double b = (dx1 * dz2 - dx2 * dz1) / det;
  const double c = sz[0] - a * sx[0] - b * sy[0];
  if (!zfreeze_plane_sane(a, b, c)) {
    zfreeze_plane_flat("slope or offset out of range");
    return;
  }
  g_gxState.zFreezePlane = {static_cast<float>(a), static_cast<float>(b), static_cast<float>(c), 0.f};
  ++g_gxState.zFreezePlaneGen;
}
} // namespace

static void handle_draw(u8 cmd, ByteReader& reader) noexcept;
static void handle_aurora(ByteReader& reader) noexcept;

ProcessResult process(const u8* data, u32 size) noexcept {
  ZoneScoped;
  ByteReader reader{{data, size}};

  while (!reader.empty()) {
    const u8 cmd = reader.read<u8>();
    u8 opcode = cmd & CP_OPCODE_MASK;

    switch (opcode) {
    case CP_CMD_NOP:
      continue;

    case CP_CMD_LOAD_BP_REG: {
      const u32 value = reader.read<u32>();
      handle_bp(value);
      if (reg_get(value, 8, 24) == GX_BP_REG_DRAWDONE) {
        return {static_cast<u32>(reader.offset()), true};
      }
      break;
    }

    case CP_CMD_LOAD_CP_REG: {
      const u8 addr = reader.read<u8>();
      handle_cp(addr, reader.read<u32>());
      break;
    }

    case CP_CMD_LOAD_XF_REG: {
      const u32 header = reader.read<u32>();
      const u32 count = ((header >> 16) & 0xFFFF) + 1;
      const u16 addr = header & 0xFFFF;
      handle_xf(addr, reader.take(count * sizeof(u32)));
      break;
    }

    case CP_CMD_LOAD_INDX_A:
    case CP_CMD_LOAD_INDX_B:
    case CP_CMD_LOAD_INDX_C:
    case CP_CMD_LOAD_INDX_D: {
      ZoneScopedN("LOAD_INDX");
      const u32 arrayType = GX_POS_MTX_ARRAY + (opcode - CP_CMD_LOAD_INDX_A) / 0x08;
      const u16 srcArrayIdx = reader.read<u16>();
      const u16 addrLen = reader.read<u16>();

      const u16 len = (addrLen >> 12) + 1;
      const u16 dstAddr = addrLen & 0x0FFF;
      auto const& array = g_gxState.arrays[arrayType];
      const u32 srcOffset = static_cast<u32>(srcArrayIdx) * array.stride;
      const u32 srcSize = static_cast<u32>(len) * sizeof(u32);
      AURORA_ASSERT(array.data != nullptr, "indexed XF load from unmapped array {}", arrayType);
      AURORA_ASSERT(srcOffset <= array.size && srcSize <= array.size - srcOffset,
                    "indexed XF load outside array {}: offset={}, size={}, array size={}", arrayType, srcOffset,
                    srcSize, array.size);
      auto const* srcData = static_cast<const u8*>(array.data) + srcOffset;
      if (!copy_xf_data(dstAddr, srcData, len, array.le ? std::endian::little : std::endian::big)) {
#ifndef NDEBUG
        Log.debug("Unimplemented indexed XF load (opcode 0x{:02X}, dstAddr=%04x)", opcode, dstAddr);
#endif
      }
      break;
    }

    case CP_CMD_CALL_DL: {
      // Call display list: 8 bytes (address + size)
      Log.warn("Ignoring nested GX_CMD_CALL_DL");
      reader.skip(8);
      break;
    }

    case CP_CMD_INVAL_VTX: {
      for (auto& array : g_gxState.arrays) {
        array.cachedRange = {};
      }
      g_gxState.dirty |= DirtyImmediates;
      break;
    }

    case GX_AURORA: {
      handle_aurora(reader);
      break;
    }

    // Draw commands: 0x80-0xBF
    case GX_DRAW_QUADS:
    case GX_DRAW_TRIANGLES:
    case GX_DRAW_TRIANGLE_STRIP:
    case GX_DRAW_TRIANGLE_FAN:
    case GX_DRAW_LINES:
    case GX_DRAW_LINE_STRIP:
    case GX_DRAW_POINTS: {
      handle_draw(cmd, reader);
      break;
    }

    default:
      // Check if it's a draw command (0x80-0xBF range)
      if (cmd >= 0x80) {
        handle_draw(cmd, reader);
      } else {
        // Hex dump surrounding bytes for debugging
        {
          const size_t pos = reader.offset();
          size_t dumpStart = (pos > 17) ? pos - 17 : 0;
          size_t dumpEnd = (pos + 16 < size) ? pos + 16 : size;
          std::string hex;
          for (size_t i = dumpStart; i < dumpEnd; i++) {
            if (i == pos - 1)
              hex += fmt::format("[{:02x}]", data[i]);
            else
              hex += fmt::format(" {:02x}", data[i]);
          }
          Log.error("  hex dump (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);
        }
        FATAL("command_processor: unknown opcode 0x{:02X} at pos {}", cmd, reader.offset() - 1);
      }
      break;
    }
  }
  return {size, false};
}

[[noreturn]] static void handle_draw_overrun(size_t totalVtxBytes, const ByteReader& reader) noexcept {
  // Hex dump around the draw command for debugging
  const size_t pos = reader.offset();
  const size_t size = reader.size();
  const u8* data = reader.data();
  size_t cmdPos = pos - 2 - 1; // opcode byte position (before vtxCount and pos++)
  size_t dumpStart = (cmdPos > 16) ? cmdPos - 16 : 0;
  size_t dumpEnd = (cmdPos + 32 < size) ? cmdPos + 32 : size;
  std::string hex;
  for (size_t i = dumpStart; i < dumpEnd; i++) {
    if (i == cmdPos)
      hex += fmt::format("[{:02x}]", data[i]);
    else
      hex += fmt::format(" {:02x}", data[i]);
  }
  Log.error("  hex dump around draw cmd (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);
  FATAL("draw vertex data overrun: need {} bytes at pos {}, have {}", totalVtxBytes, pos, reader.remaining());
}

static u32 calc_vtx_size(GXVtxFmt fmt) noexcept {
  u32 vtxSize = 0;
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto& attrFmt = vtxFmt.attrs[i];
    switch (g_gxState.vtxDesc[i]) {
    case GX_NONE:
      break;
    case GX_DIRECT: {
      const auto attr = static_cast<GXAttr>(i);
      vtxSize += comp_type_size(attr, attrFmt.type) * comp_cnt_count(attr, attrFmt.cnt);
      break;
    }
    case GX_INDEX8:
      vtxSize += i == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3 ? 3 : 1;
      break;
    case GX_INDEX16:
      vtxSize += i == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3 ? 6 : 2;
      break;
    }
  }
  g_gxState.lastVtxFmt = fmt;
  g_gxState.lastVtxSize = vtxSize;
  return vtxSize;
}

static void push_gx_draw(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange, gfx::Range idxRange,
                         u32 numIndices) noexcept {
  auto& state = g_gxState;
  auto& cache = sDrawCache;

  // GX_CULL_ALL discards every polygon (lines and points are not culled); WebGPU has no
  // equivalent cull mode, so such draws are simply not issued.
  if (state.cullMode == GX_CULL_ALL && line_mode_for_prim(prim) == 0) {
    return;
  }

  DrawImmediateData immediates{.vtxStart = vertRange.offset, .currentPnMtx = state.currentPnMtx};
  for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
    if (state.vtxDesc[i] != GX_INDEX8 && state.vtxDesc[i] != GX_INDEX16) {
      continue;
    }
    auto& array = state.arrays[i];
    if (array.cachedRange.size == 0) {
      array.cachedRange = gfx::push_storage(static_cast<const uint8_t*>(array.data), array.size);
    }
    immediates.arrayStart[i - GX_VA_POS] = array.cachedRange.offset;
  }

  const u8 lineMode = line_mode_for_prim(prim);
  const auto targetLayoutKey = gfx::get_render_target_layout().key;
  const bool pipelineValid = cache.hasPipeline && (state.dirty & DirtyPipeline) == 0 && cache.fmt == fmt &&
                             cache.lineMode == lineMode && cache.targetLayoutKey == targetLayoutKey;
  if (!pipelineValid) {
    const bool hadPipeline = cache.hasPipeline;
    const auto prevSampledTextures = cache.shaderInfo.sampledTextures;
    const auto prevSampledIndTextures = cache.shaderInfo.sampledIndTextures;
    populate_pipeline_config(cache.config, prim, fmt);
    cache.shaderInfo = build_shader_info(cache.config.shaderConfig);
#ifndef NDEBUG
    validate_shader_config(cache.config.shaderConfig, cache.shaderInfo, g_gxState.numTexGens);
#endif
    cache.pipelineRef = gfx::pipeline_ref(cache.config);
    // Every fixed-function state seen gets its uber pipeline (remembered in the pipeline cache, so
    // the startup precompile builds it): a shader never seen before then has one to stand in.
    cache.uberRef = g_uberMode != UberMode::Off
                        ? gfx::find_pipeline(uber_pipeline_config(cache.config), gfx::get_render_target_layout())
                        : gfx::PipelineRef{};
    cache.specializedReady = false;
    cache.targetLayoutKey = targetLayoutKey;
    cache.fmt = fmt;
    cache.lineMode = lineMode;
    cache.hasPipeline = true;
    state.dirty = (state.dirty & ~DirtyPipeline) | DirtyUniform;
    if (!hadPipeline || prevSampledTextures != cache.shaderInfo.sampledTextures ||
        prevSampledIndTextures != cache.shaderInfo.sampledIndTextures) {
      cache.bindGeneration = 0;
    }
  }

  const bool bindGroupsValid =
      (state.dirty & DirtyTextures) == 0 && cache.bindGeneration == texture::current_bind_generation();
  if (!bindGroupsValid) {
    const auto prevBindGroup = cache.bindGroups.textureBindGroup;
    resolve_sampled_textures(cache.shaderInfo);
    cache.bindGroups = build_bind_groups(cache.shaderInfo);
    cache.bindGeneration = texture::current_bind_generation();
    state.dirty &= ~DirtyTextures;
    // For texture_size_bias uniform
    if (cache.bindGroups.textureBindGroup != prevBindGroup) {
      state.dirty |= DirtyUniform;
    }
  }

  // Hybrid ubershaders: until the specialized pipeline is built, the draw uses the ubershader
  // (its own uniform block) rather than being skipped.
  bool useUber = false;
  if (g_uberMode == UberMode::Exclusive) {
    useUber = true;
  } else if (g_uberMode == UberMode::Hybrid) {
    if (!cache.specializedReady) {
      cache.specializedReady = gfx::pipeline_ready(cache.pipelineRef);
    }
    useUber = !cache.specializedReady && gfx::pipeline_ready(cache.uberRef);
  }
  if (useUber != cache.uniformUber) {
    state.dirty |= DirtyUniform;
  }
  if (useUber) {
    std::atomic_ref{gfx::detail::resources().stats.uberDraws}.fetch_add(1, std::memory_order_relaxed);
  }

  const bool uniformValid = (state.dirty & DirtyUniform) == 0 && cache.uniformRange.size != 0;
  if (!uniformValid) {
    cache.uniformRange = useUber ? build_uber_uniform(cache.config.shaderConfig, cache.shaderInfo)
                                 : build_uniform(cache.shaderInfo);
    cache.uniformUber = useUber;
    cache.zFreezePlaneGen = state.zFreezePlaneGen;
    cache.nbtCacheGen = state.nbtCacheGen;
    state.dirty &= ~DirtyUniform;
  }
  if (cache.config.shaderConfig.fogRangeEnabled) {
    const auto key = fog_range_lut_key();
    if (!cache.hasFogRange || cache.fogRangeKey != key) {
      cache.fogRange = push_fog_range_lut(key);
      cache.fogRangeKey = key;
      cache.hasFogRange = true;
    }
  }
  immediates.fogRangeBase = cache.fogRange.offset / sizeof(u32);

  state.dirty &= ~DirtyImmediates;

  uint32_t instanceCount = 1;
  if (prim == GX_LINES) {
    instanceCount = vtxCount / 2;
  } else if (prim == GX_LINESTRIP) {
    instanceCount = vtxCount - 1;
  } else if (prim == GX_POINTS) {
    instanceCount = vtxCount;
  }
  cache.lastDrawFmt = fmt;
  gfx::push_draw_command(DrawData{
      .pipeline = useUber ? cache.uberRef : cache.pipelineRef,
      .bindGroups = cache.bindGroups,
      .vertRange = vertRange,
      .idxRange = idxRange,
      .uniformRange = cache.uniformRange,
      .immediateData = immediates,
      .vtxCount = vtxCount,
      .indexCount = numIndices,
      .instanceCount = instanceCount,
      .dstAlpha = state.dstAlpha,
  });
}

static void handle_draw_unmerged(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange) noexcept {
  ZoneScoped;
  u32 numIndices = 0;
  gfx::Range idxRange;

  if (prim != GX_TRIANGLES) {
    ZoneScopedN("build idx buffer");
    static ByteBuffer idxBuf;
    numIndices = prepare_idx_buffer(idxBuf, prim, 0, vtxCount);
    idxRange = gfx::push_indices(idxBuf.data(), idxBuf.size(), 4);
    idxBuf.clear();
  }

  push_gx_draw(prim, fmt, vtxCount, vertRange, idxRange, numIndices);
}

// Truncate vertex count to a valid number for the given primitive type
static constexpr u16 trunc_vtx_count(GXPrimitive prim, u16 vtxCount) noexcept {
  switch (prim) {
  case GX_QUADS:
    return vtxCount & ~3;
  case GX_TRIANGLES:
    return vtxCount - vtxCount % 3;
  case GX_TRIANGLESTRIP:
  case GX_TRIANGLEFAN:
    return vtxCount < 3 ? 0 : vtxCount;
  case GX_LINES:
    return vtxCount & ~1;
  case GX_LINESTRIP:
    return vtxCount < 2 ? 0 : vtxCount;
  default:
    return vtxCount;
  }
}

static void draw_prim(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, ByteReader& reader) noexcept {
  ZoneScoped;
  u32 vtxSize;
  if (g_gxState.lastVtxFmt == fmt)
    LIKELY { vtxSize = g_gxState.lastVtxSize; }
  else
    UNLIKELY { vtxSize = calc_vtx_size(fmt); }

  u32 totalVtxBytes = vtxCount * vtxSize;
  if (totalVtxBytes > reader.remaining())
    UNLIKELY { handle_draw_overrun(totalVtxBytes, reader); }
  const auto vertexData = reader.take(totalVtxBytes);

  vtxCount = trunc_vtx_count(prim, vtxCount);
  if (vtxCount == 0)
    UNLIKELY { return; }

  // Too few vertices for a single primitive: hardware draws nothing. Consume the data and drop
  // the draw, so it cannot become part of a merged batch either.
  // (A quad needs three: see prepare_idx_buffer.)
  const u16 minVerts = (prim == GX_LINES || prim == GX_LINESTRIP) ? 2
                       : prim == GX_POINTS ? 1
                                           : 3;
  if (vtxCount < minVerts) {
    reader.take(totalVtxBytes);
    return;
  }

  const u8* verts = reader.data() + reader.offset();
  // A draw with nothing changed since the previous one (glyphs of a text, pieces of an element)
  // keeps its widescreen scales: no per-draw work, and an element moves as one.
  // (Screen-position sprites each have their own center, draws filling the screen their own
  // extent: always.)
  const bool unchanged = g_gxState.dirty == 0 && fmt == sDrawCache.lastDrawFmt && !g_gxState.fillScreen &&
                         !is_screen_sprite(static_cast<GXPrimitive>(prim), vtxCount);
  if (!unchanged) {
    auto ps = widescreen_scale(fmt, verts, vtxCount, vtxSize, static_cast<GXPrimitive>(prim));
    if (g_gxState.fillScreen) {
      ps = fill_screen_scale(ps, fmt, verts, vtxCount, vtxSize);
    }
    if (ps.x != g_gxState.projXScale || ps.y != g_gxState.projYScale || ps.xOffset != g_gxState.projXOffsetScale ||
        ps.xAdd != g_gxState.projXAdd || ps.yAdd != g_gxState.projYAdd) {
      g_gxState.projXScale = ps.x;
      g_gxState.projYScale = ps.y;
      g_gxState.projXOffsetScale = ps.xOffset;
      g_gxState.projXAdd = ps.xAdd;
      g_gxState.projYAdd = ps.yAdd;
      g_gxState.dirty |= DirtyUniform;
    }
  }

  const bool triangles =
      prim == GX_QUADS || prim == GX_TRIANGLES || prim == GX_TRIANGLESTRIP || prim == GX_TRIANGLEFAN;
  if (g_gxState.zFreeze) {
    update_zfreeze_plane();
    if (sDrawCache.zFreezePlaneGen != g_gxState.zFreezePlaneGen) {
      g_gxState.dirty |= DirtyUniform;
    }
    // RL-NX (R2_ZFREEZE_LOG=1): zfreeze is the only place in this pipeline that writes one constant depth
    // across a whole quad - clamp(plane(x, y), 0, 1) at every pixel - and the RL-NX flicker's depth dumps are
    // a single value over the picture's whole rect (9.30). This names the draws that use it and says what the
    // plane evaluates to: a plane that clamps to 1.0 writes far depth across the viewport and every depth-tested
    // draw after it is rejected, while the undepth-tested stars, sprites and HUD still paint - the flicker's
    // recorded picture. Diagnostic only.
      static const bool zfLog = getenv("R2_ZFREEZE_LOG") != nullptr;
      if (zfLog) {
        ++s_zfreezeDraws;
        const auto& vp = g_gxState.renderViewport;
        const auto& plane = g_gxState.zFreezePlane;
        const float pa = plane.m[0], pb = plane.m[1], pc = plane.m[2];
        const float cx = vp.left + vp.width * 0.5f;
        const float cy = vp.top + vp.height * 0.5f;
        const auto at = [&](float x, float y) { return std::clamp(pa * x + pb * y + pc, 0.f, 1.f); };
        if (s_zfreezeLogged < 12 || s_zfreezeDraws % 600 == 0) {
          ++s_zfreezeLogged;
          Log.info("zfreeze draw {}: plane {:.4f} {:.4f} {:.4f}, viewport {}x{} at ({}, {}), gen {}", s_zfreezeDraws,
                   pa, pb, pc, vp.width, vp.height, vp.left, vp.top, g_gxState.zFreezePlaneGen);
          Log.info("  depth at centre ({:.0f}, {:.0f}) {:.4f}, top edge {} bottom edge {:.4f} right edge {} (1.0 = "
                   "far under reversed depth: nothing behind it passes)",
                   cx, cy, at(cx, cy), at(cx, vp.top), at(cx, vp.top + vp.height), at(vp.left + vp.width, cy));
        }
      }
  } else if (triangles) {
    // before any culling: the reference triangle is often drawn with GX_CULL_ALL
    note_zfreeze_reference(fmt, verts + (vtxCount - 3) * vtxSize, vtxSize);
  }
  // binormal / tangent left behind for draws without them (before culling, like zfreeze's)
  note_nbt(fmt, verts + (vtxCount - 1) * vtxSize);
  if (sDrawCache.nbtCacheGen != g_gxState.nbtCacheGen && reads_nbt_cache(fmt)) {
    g_gxState.dirty |= DirtyUniform;
  }

  const bool cleanState = g_gxState.dirty == 0 && fmt == sDrawCache.lastDrawFmt && sDrawCache.lineMode == 0 &&
                          prim != GX_LINES && prim != GX_LINESTRIP && prim != GX_POINTS;
  auto* lastDraw = cleanState ? gfx::get_last_draw_command<DrawData>() : nullptr;
  const bool canMerge = lastDraw != nullptr && lastDraw->instanceCount == 1;

  // The frame's buffers are fixed-size: a draw that would overflow one is dropped (and counted)
  // rather than ending the program. Index bound: lines expand to 12 bytes a vertex at most, and
  // a merge may index the previous plain triangle list as well.
  {
    size_t storageNeed = 64 * 1024;
    for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
      if ((g_gxState.vtxDesc[i] == GX_INDEX8 || g_gxState.vtxDesc[i] == GX_INDEX16) &&
          g_gxState.arrays[i].cachedRange.size == 0) {
        storageNeed += g_gxState.arrays[i].size + 256;
      }
    }
    const size_t idxNeed = size_t(vtxCount) * 12 + (canMerge ? size_t(lastDraw->vtxCount) * 2 : 0) + 64 * 1024;
    if (!gfx::frame_has_room(totalVtxBytes + 64 * 1024, idxNeed, 64 * 1024, storageNeed)) {
      reader.take(totalVtxBytes);
      gfx::note_dropped_draw();
      return;
    }
  }

  // Push raw vertex data to buffer. Merged draws must remain contiguous with the previous range.
  gfx::Range vertRange = gfx::push_verts(vertexData.data(), vtxCount * vtxSize, canMerge ? 0 : 4);

  // Try to merge with previous draw call
  if (canMerge) {
    u32 numIndices = 0;
    gfx::Range idxRange;
    static ByteBuffer idxBuf;
    const bool hadIndexRange = lastDraw->idxRange.size != 0;
    // The merged draw stays non-indexed only if both parts are plain lists of whole triangles;
    // otherwise index both (a previous draw without indices may also be an empty strip/fan,
    // whose leftover vertices must not pair up with this draw's).
    const bool lastPlain = lastDraw->indexCount == 0 && !hadIndexRange;
    const bool indexed = !lastPlain || prim != GX_TRIANGLES || (lastDraw->vtxCount % 3) != 0;
    if (indexed && lastPlain) {
      // Generate triangle index buffer for previous draw
      lastDraw->indexCount = prepare_idx_buffer(idxBuf, GX_TRIANGLES, 0, lastDraw->vtxCount);
    }
    if (indexed) {
      numIndices += prepare_idx_buffer(idxBuf, prim, lastDraw->vtxCount, vtxCount);
      idxRange = gfx::push_indices(idxBuf.data(), idxBuf.size(), hadIndexRange ? 0 : 4);
      idxBuf.clear();
    }
    CHECK(lastDraw->vertRange.offset + lastDraw->vertRange.size == vertRange.offset,
          "Non-consecutive vertex ranges ({} < {})", lastDraw->vertRange.offset + lastDraw->vertRange.size,
          vertRange.offset);
    if (hadIndexRange) {
      CHECK(lastDraw->idxRange.offset + lastDraw->idxRange.size == idxRange.offset,
            "Non-consecutive index ranges ({} < {})", lastDraw->idxRange.offset + lastDraw->idxRange.size,
            idxRange.offset);
    }
    lastDraw->vertRange.size += vertRange.size;
    if (lastDraw->idxRange.size == 0) {
      lastDraw->idxRange = idxRange;
    } else {
      lastDraw->idxRange.size += idxRange.size;
    }
    lastDraw->vtxCount += vtxCount;
    lastDraw->indexCount += numIndices;
    gfx::detail::increment_merged_draw_count();
    return;
  }

  handle_draw_unmerged(prim, fmt, vtxCount, vertRange);
}

static void handle_draw(u8 cmd, ByteReader& reader) noexcept {
  const auto fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
  const auto prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
  draw_prim(prim, fmt, reader.read<u16>(), reader);
}

void handle_aurora(ByteReader& reader) noexcept {
  ZoneScoped;
  const u16 subCmd = reader.read<u16>();

  if (subCmd == GX_AURORA_LOAD_VIEWPORT_RENDER) {
    const f32 left = reader.read<f32>();
    const f32 top = reader.read<f32>();
    const f32 width = reader.read<f32>();
    const f32 height = reader.read<f32>();
    const f32 nearZ = reader.read<f32>();
    const f32 farZ = reader.read<f32>();
    set_render_viewport({
        .left = left,
        .top = top,
        .width = width,
        .height = height,
        .znear = nearZ,
        .zfar = farZ,
    });
  } else if (subCmd == GX_AURORA_LOAD_SCISSOR_RENDER) {
    const s32 left = reader.read<s32>();
    const s32 top = reader.read<s32>();
    const s32 width = reader.read<s32>();
    const s32 height = reader.read<s32>();
    set_render_scissor({left, top, width, height});
  } else if (subCmd == GX_AURORA_LOAD_PROJECTION_FULL) {
    auto& proj = g_gxState.proj;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        proj[r][c] = reader.read<f32>();
      }
    }
    // Invalidate projection XF regs
    for (u32 reg = 0x20; reg <= 0x26; ++reg) {
      g_gxState.xfRegValid.reset(reg);
    }
    g_gxState.dirty |= DirtyUniform;
  } else if (subCmd >= GX_AURORA_LOAD_ARRAYBASE && subCmd <= (GX_AURORA_LOAD_ARRAYBASE | 0x0f)) {
    const u32 attrIdx = subCmd - GX_AURORA_LOAD_ARRAYBASE + GX_VA_POS;
    const u64 arrayAddr = reader.read<u64>();
    const u32 arraySize = reader.read<u32>();
    const bool le = reader.read<u8>() == 1;

    auto& array = g_gxState.arrays[attrIdx];
    const auto newData = reinterpret_cast<void*>(arrayAddr);
    if (array.data != newData || array.size != arraySize || array.le != le) {
      if (array.le != le) {
        // Endianness is baked into the shader
        g_gxState.dirty |= DirtyPipeline;
      }
      array.data = newData;
      array.size = arraySize;
      array.le = le;
      array.cachedRange = {};
      g_gxState.dirty |= DirtyImmediates;
    }
  } else if (subCmd == GX_AURORA_LOAD_TEXOBJ) {
    const auto texMapId = reader.read<u8>();
    CHECK(texMapId < MaxTextures, "invalid texture map id {}", texMapId);
    auto& slot = g_gxState.loadedTextures[texMapId];
    const auto newData = reinterpret_cast<const void*>(reader.read<u64>());
    const u32 newWidth = reader.read<u32>();
    const u32 newHeight = reader.read<u32>();
    const auto newFormat = static_cast<GXTexFmt>(reader.read<u32>());
    const auto newTlut = static_cast<GXTlut>(reader.read<u32>());
    u8 newFlags = slot.flags & ~0x80u; // Reset no-cache flag
    if (reader.read<u8>() != 0) {
      newFlags |= 1u;
    } else {
      newFlags &= ~1u;
    }
    const u32 newTexObjId = reader.read<u32>();
    const u32 newTexDataVersion = reader.read<u32>();
    if (slot.data != newData || slot.mWidth != newWidth || slot.mHeight != newHeight ||
        slot.mFormat != static_cast<u32>(newFormat) || slot.tlut != newTlut || slot.flags != newFlags ||
        slot.texObjId != newTexObjId || slot.texDataVersion != newTexDataVersion) {
      slot.data = newData;
      slot.mWidth = newWidth;
      slot.mHeight = newHeight;
      slot.mFormat = newFormat;
      slot.tlut = newTlut;
      slot.flags = newFlags;
      slot.texObjId = newTexObjId;
      slot.texDataVersion = newTexDataVersion;
      g_gxState.dirty |= DirtyTextures;
    }
  } else if (subCmd == GX_AURORA_SET_TEX_IMG_PTR) {
    const auto texMapId = reader.read<u8>();
    CHECK(texMapId < MaxTextures, "invalid texture map id {}", texMapId);
    auto& slot = g_gxState.loadedTextures[texMapId];
    const auto newData = reinterpret_cast<const void*>(reader.read<u64>());
    if (slot.data != newData) {
      slot.data = newData;
      slot.texObjId = 0;
      slot.texDataVersion = 0;
      g_gxState.dirty |= DirtyTextures;
    }
  } else if (subCmd == GX_AURORA_LOAD_TLUT) {
    const auto idx = reader.read<u8>();
    CHECK(idx < MaxTluts, "invalid tlut slot {}", idx);
    auto& slot = g_gxState.loadedTluts[idx];
    const auto newData = reinterpret_cast<const void*>(reader.read<u64>());
    const auto newFormat = static_cast<GXTlutFmt>(reader.read<u32>());
    const u16 newNumEntries = reader.read<u16>();
    const u32 newTlutObjId = reader.read<u32>();
    const u32 newTlutDataVersion = reader.read<u32>();
    const u8 newFlags = slot.flags & ~0x80u; // Reset no-cache flag
    if (slot.data != newData || slot.format != newFormat || slot.numEntries != newNumEntries ||
        slot.tlutObjId != newTlutObjId || slot.tlutDataVersion != newTlutDataVersion || slot.flags != newFlags) {
      if (slot.tlutObjId != newTlutObjId || slot.tlutDataVersion != newTlutDataVersion) {
        texture::invalidate_bindings();
      }
      slot.data = newData;
      slot.format = newFormat;
      slot.numEntries = newNumEntries;
      slot.tlutObjId = newTlutObjId;
      slot.tlutDataVersion = newTlutDataVersion;
      slot.flags = newFlags;
      g_gxState.dirty |= DirtyTextures;
    }
  } else if (subCmd == GX2_SET_POLYGON_OFFSET) {
    const f32 frontOffset = reader.read<f32>();
    const f32 frontScale = reader.read<f32>();
    const f32 backOffset = reader.read<f32>();
    const f32 backScale = reader.read<f32>();
    const f32 clamp = reader.read<f32>();
    if (g_gxState.frontOffset != frontOffset || g_gxState.frontScale != frontScale ||
        g_gxState.backOffset != backOffset || g_gxState.backScale != backScale || g_gxState.clamp != clamp) {
      g_gxState.frontOffset = frontOffset;
      g_gxState.frontScale = frontScale;
      g_gxState.backOffset = backOffset;
      g_gxState.backScale = backScale;
      g_gxState.clamp = clamp;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_SET_DRAWS_BLOCKING) {
    const bool blocking = reader.read<u8>() != 0;
    if (g_gxState.drawsBlocking != blocking) {
      g_gxState.drawsBlocking = blocking;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_LOAD_COPY_SRC) {
    const s32 left = reader.read<s32>();
    const s32 top = reader.read<s32>();
    const s32 width = reader.read<s32>();
    const s32 height = reader.read<s32>();
    g_gxState.texCopySrc = {left, top, width, height};
  } else if (subCmd == GX_AURORA_LOAD_COPY_DST) {
    g_gxState.texCopyDstWidth = reader.read<u32>();
    g_gxState.texCopyDstHeight = reader.read<u32>();
    g_gxState.texCopyFmt = static_cast<GXTexFmt>(reader.read<u32>());
    reader.skip(1); // mipmap is not implemented, but remains part of the command payload
    g_gxState.texCopyDstWide = true;
  } else if (subCmd == GX_AURORA_LOAD_COPY_DEST) {
    g_gxState.texCopyDest = reinterpret_cast<const void*>(reader.read<u64>());
  } else if (subCmd == GX_AURORA_LOAD_DISP_COPY_SRC) {
    const s32 left = reader.read<s32>();
    const s32 top = reader.read<s32>();
    const s32 width = reader.read<s32>();
    const s32 height = reader.read<s32>();
    g_gxState.dispCopySrc = {left, top, width, height};
  } else if (subCmd == GX_AURORA_LOAD_DISP_COPY_DST) {
    g_gxState.dispCopyDstY = reader.read<s32>();
    g_gxState.xfbHeight = reader.read<u32>();
  } else if (subCmd == GX_AURORA_SET_SCREEN_SPACE_2D) {
    const bool on = reader.read<u8>() != 0;
    if (on != g_gxState.screenSpace2D) {
      g_gxState.screenSpace2D = on;
      g_gxState.dirty |= DirtyUniform; // (the next draw decides its widescreen scales again)
    }
  } else if (subCmd == GX_AURORA_SET_FILL_SCREEN) {
    const bool on = reader.read<u8>() != 0;
    const f32 shiftX = reader.read<f32>();
    const f32 shiftY = reader.read<f32>();
    if (on != g_gxState.fillScreen || shiftX != g_gxState.fillShiftX || shiftY != g_gxState.fillShiftY) {
      g_gxState.fillScreen = on;
      g_gxState.fillShiftX = shiftX;
      g_gxState.fillShiftY = shiftY;
      g_gxState.dirty |= DirtyUniform; // (the next draw decides its scales again)
    }
  } else if (subCmd == GX_AURORA_READBACK_CAPTURE_PEEKS) {
    readback::handle_capture_peeks(reader);
  } else if (subCmd == GX_AURORA_READBACK_CAPTURE_COPIES) {
    readback::handle_capture_copies(reader);
  } else if (subCmd == GX_AURORA_BEGIN_OFFSCREEN) {
    const u32 width = reader.read<u32>();
    const u32 height = reader.read<u32>();
    gfx::begin_offscreen(width, height);
  } else if (subCmd == GX_AURORA_END_OFFSCREEN) {
    gfx::end_offscreen();
  } else if (subCmd == GX_AURORA_DESTROY_TEXOBJ) {
    evict_texture_object(reader.read<u32>());
  } else if (subCmd == GX_AURORA_DESTROY_TLUT) {
    evict_tlut_object(reader.read<u32>());
  } else if (subCmd == GX_AURORA_DESTROY_COPY_TEX) {
    evict_copy_texture(reinterpret_cast<const void*>(reader.read<u64>()));
  } else if (subCmd == GX_AURORA_DRAW_SIZED) {
    const u8 cmd = reader.read<u8>();
    const u32 byteLen = reader.read<u32>();
    const GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
    const GXPrimitive prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    if (byteLen != 0) {
      u32 vtxSize;
      if (g_gxState.lastVtxFmt == fmt) {
        vtxSize = g_gxState.lastVtxSize;
      } else {
        vtxSize = calc_vtx_size(fmt);
      }
      AURORA_ASSERT(vtxSize != 0 && byteLen % vtxSize == 0,
                    "GX_AURORA_DRAW_SIZED: {} bytes is not a whole number of size-{} vertices", byteLen, vtxSize);
      u32 vtxCount = byteLen / vtxSize;
      AURORA_ASSERT(vtxCount <= 0xFFFF, "GX_AURORA_DRAW_SIZED: too many vertices ({})", vtxCount);
      draw_prim(prim, fmt, static_cast<u16>(vtxCount), reader);
    }
  } else if (subCmd == GX_AURORA_DRAW_INDEXED) {
    ZoneScopedN("DRAW_INDEXED");
    const u8 cmd = reader.read<u8>();
    const u16 vtxCount = reader.read<u16>();
    const u32 indexCount = reader.read<u32>();
    const GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
    const GXPrimitive prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    AURORA_ASSERT(prim == GX_TRIANGLES, "GX_AURORA_DRAW_INDEXED: primitive must be GX_TRIANGLES, got {}",
                  static_cast<u32>(prim));
    const size_t idxBytes = static_cast<size_t>(indexCount) * sizeof(u16);
    // Index data is always host-endian; push it to the GPU buffer as-is
    const auto indexData = reader.take(idxBytes);
    const gfx::Range idxRange = gfx::push_indices(indexData.data(), indexData.size(), 4);
    u32 vtxSize;
    if (g_gxState.lastVtxFmt == fmt) {
      vtxSize = g_gxState.lastVtxSize;
    } else {
      vtxSize = calc_vtx_size(fmt);
    }
    const u32 totalVtxBytes = vtxCount * vtxSize;
    const auto vertexData = reader.take(totalVtxBytes);
    const gfx::Range vertRange = gfx::push_verts(vertexData.data(), vertexData.size(), 4);
    if (indexCount != 0) {
      push_gx_draw(prim, fmt, vtxCount, vertRange, idxRange, indexCount);
    }
  } else if (subCmd == GX_AURORA_DEBUG_GROUP_PUSH) {
    auto label = reader.read_string();
    gfx::push_debug_group(std::move(label));
  } else if (subCmd == GX_AURORA_DEBUG_GROUP_POP) {
    pop_debug_group();
  } else if (subCmd == GX_AURORA_DEBUG_MARKER_INSERT) {
    auto label = reader.read_string();
    gfx::insert_debug_marker(std::move(label));
  } else if (subCmd == GX_AURORA_CLEAR) {
    const auto flags = reader.read<u8>();
    const gfx::ClipRect rect{
        .x = static_cast<int32_t>(reader.read<u32>()),
        .y = static_cast<int32_t>(reader.read<u32>()),
        .width = static_cast<int32_t>(reader.read<u32>()),
        .height = static_cast<int32_t>(reader.read<u32>()),
    };
    const GXColor color{reader.read<u8>(), reader.read<u8>(), reader.read<u8>(), reader.read<u8>()};
    const auto z = reader.read<u32>();
    clear_efb(rect, (flags & 0x08) != 0, (flags & 0x01) != 0, (flags & 0x02) != 0, (flags & 0x04) != 0, color, z);
  }

  else {
    Log.error("Unknown Aurora subcommand: {:04X}", subCmd);
  }
}

void clear_draw_cache() noexcept {
  sDrawCache.bindGeneration = 0;
  sDrawCache.uniformRange = {};
  sDrawCache.fogRange = {};
  sDrawCache.hasFogRange = false;
}

} // namespace aurora::gx::fifo
