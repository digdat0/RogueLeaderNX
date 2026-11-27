#include "gx.hpp"
#include "__gx.h"

#include "../../gfx/texture.hpp"
#include "../../gfx/recording.hpp"
#include "../../window.hpp"
#include "../../webgpu/gpu.hpp"
#include "../../gx/texture.hpp"
#include "../vi/vi_internal.hpp"

#include <algorithm>
#include <cmath>

namespace {
aurora::Vec2<uint32_t> scale_copy_dst(u32 logicalWidth, u32 logicalHeight) {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return {logicalWidth, logicalHeight};
  }

  const auto [logicalFbWidth, logicalFbHeight] = aurora::gx::logical_fb_size();
  const auto [targetWidth, targetHeight] = aurora::gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return {logicalWidth, logicalHeight};
  }

  const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
  const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);
  const auto scaledWidth = std::max<u32>(static_cast<u32>(std::lround(static_cast<float>(logicalWidth) * scaleX)), 1);
  const auto scaledHeight = std::max<u32>(static_cast<u32>(std::lround(static_cast<float>(logicalHeight) * scaleY)), 1);
  return {scaledWidth, scaledHeight};
}

// GX textures are stored in tiles: the texel rows of one row of tiles and its bytes, for a
// texture `width` texels wide. False for formats without a fixed tiling here.
bool tile_rows(u32 format, u32 width, u32& rows, u32& rowBytes) {
  u32 tileW = 4, tileBytes = 32;
  switch (format) {
  case GX_TF_I4: case GX_TF_C4: case GX_TF_CMPR: case GX_CTF_R4: case GX_CTF_Z4:
    tileW = 8, rows = 8;
    break;
  case GX_TF_I8: case GX_TF_IA4: case GX_TF_C8: case GX_CTF_RA4: case GX_CTF_A8: case GX_CTF_R8: case GX_CTF_G8:
  case GX_CTF_B8: case GX_TF_Z8: case GX_CTF_Z8M: case GX_CTF_Z8L:
    tileW = 8, rows = 4;
    break;
  case GX_TF_IA8: case GX_TF_RGB565: case GX_TF_RGB5A3: case GX_TF_C14X2: case GX_CTF_RA8: case GX_CTF_RG8:
  case GX_CTF_GB8: case GX_TF_Z16: case GX_CTF_Z16L:
    rows = 4;
    break;
  case GX_TF_RGBA8: case GX_TF_Z24X8:
    rows = 4, tileBytes = 64;
    break;
  default:
    return false;
  }
  rowBytes = (width + tileW - 1) / tileW * tileBytes;
  return true;
}

// A texture copy whose destination lies inside an earlier copy's texture in memory (same format
// and width, starting on one of its rows of tiles) writes those rows of it, as on hardware: the
// copy only overwrites that part of the texture. Rogue Leader copies the EFB at half size for
// the Maw prison's force field (a 320x240 refraction texture), then two 640x16 bands into its
// first and last 8 rows; each band became a texture of its own at that address, and the field
// sampled a 320x8 strip stretched over its whole surface.
// Only copies made in the current frame count: an older one may be memory the game has reused.
aurora::gx::GXState::CopyTextureRef* enclosing_copy(const void* dest, u32 width, u32 height, GXTexFmt format,
                                                    u32& firstRow) {
  u32 tileRows = 0, rowBytes = 0;
  if (!tile_rows(format, width, tileRows, rowBytes) || rowBytes == 0) {
    return nullptr;
  }
  const auto* d = static_cast<const u8*>(dest);
  const uint64_t frame = aurora::gx::texture::frame_count();
  aurora::gx::GXState::CopyTextureRef* best = nullptr;
  for (auto& [addr, ref] : g_gxState.copyTextures) {
    if (!ref || ref.frame != frame || ref.key.format != format || ref.width != width || ref.height <= height) {
      continue;
    }
    const auto* base = static_cast<const u8*>(addr);
    if (d < base) {
      continue;
    }
    const size_t offset = static_cast<size_t>(d - base);
    if (offset % rowBytes != 0) {
      continue;
    }
    const size_t row = offset / rowBytes * tileRows;
    if (row + height > ref.height || (best != nullptr && best->height >= ref.height)) {
      continue;
    }
    best = &ref;
    firstRow = static_cast<u32>(row);
  }
  return best;
}
} // namespace

namespace aurora::gx {
void copy_tex(const void* dest, GXBool clear) noexcept {
  {
    // remembered: draws into this rectangle (from the next frame on) render a texture
    const auto& src = g_gxState.texCopySrc;
    auto& rects = g_gxState.rttRects;
    const bool known = std::any_of(rects.begin(), rects.end(), [&](const gfx::ClipRect& r) {
      return r.x == src.x && r.y == src.y && r.width == src.width && r.height == src.height;
    });
    if (!known) {
      rects[g_gxState.rttRectNext++ % rects.size()] = src;
    }
  }
  const auto rect = map_logical_scissor(g_gxState.texCopySrc);
  u32 firstRow = 0;
  if (auto* outer = enclosing_copy(dest, g_gxState.texCopyDstWidth, g_gxState.texCopyDstHeight,
                                   g_gxState.texCopyFmt, firstRow)) {
    const auto& size = outer->handle->size;
    const float scaleY = static_cast<float>(size.height) / static_cast<float>(outer->height);
    const auto top = static_cast<int32_t>(std::lround(static_cast<float>(firstRow) * scaleY));
    const auto bottom = static_cast<int32_t>(
        std::lround(static_cast<float>(firstRow + g_gxState.texCopyDstHeight) * scaleY));
    if (bottom > top) {
      ++outer->revision;
      const auto clearColor = clear && g_gxState.colorUpdate;
      const auto clearAlpha = clear && g_gxState.alphaUpdate && efb_has_alpha(g_gxState.pixelFmt);
      const auto clearDepth = clear && g_gxState.depthUpdate;
      gfx::resolve_pass_into(outer->handle, rect, clearColor, clearAlpha, clearDepth, g_gxState.clearColor,
                             clear_depth_value(), g_gxState.texCopyFmt, g_gxState.pixelFmt,
                             {0, top, static_cast<int32_t>(size.width), bottom - top});
      texture::invalidate_bindings();
      return;
    }
  }
  const auto [dstWidth, dstHeight] = scale_copy_dst(g_gxState.texCopyDstWidth, g_gxState.texCopyDstHeight);
  const auto texCopyFmt = g_gxState.texCopyFmt;
  const auto copy = texture::resolve_copy_texture({
      .dest = dest,
      .width = dstWidth,
      .height = dstHeight,
      .format = texCopyFmt,
  });
  auto& current = g_gxState.copyTextures[dest];
  current.width = g_gxState.texCopyDstWidth;
  current.height = g_gxState.texCopyDstHeight;
  current.pixelFormat = g_gxState.pixelFmt;

  const auto clearColor = clear && g_gxState.colorUpdate;
  const auto clearAlpha = clear && g_gxState.alphaUpdate && efb_has_alpha(g_gxState.pixelFmt);
  const auto clearDepth = clear && g_gxState.depthUpdate;
  gfx::resolve_pass_into(copy.handle, rect, clearColor, clearAlpha, clearDepth, g_gxState.clearColor,
                         clear_depth_value(), texCopyFmt, g_gxState.pixelFmt);
}

void copy_disp(GXBool clear) noexcept {
  if (!g_config.emulateXfb) {
    return;
  }

  const auto rect = map_logical_scissor(g_gxState.dispCopySrc);
  const auto clearColor = clear && g_gxState.colorUpdate;
  const auto clearAlpha = clear && g_gxState.alphaUpdate && efb_has_alpha(g_gxState.pixelFmt);
  const auto clearDepth = clear && g_gxState.depthUpdate;
  // Destination within a composed XFB (logical -> host pixels). Every copy into it takes the
  // frame buffer's one scale: a scale from each copy's own rounded rect differs between copies
  // at fractional scales (900 / 480: the top half's rect 424 rows, the bottom's 425), and the
  // XFB height following it changed between the halves of one frame, so the second half's copy
  // started a new XFB and the first half was lost.
  s32 dstY = 0;
  u32 xfbHeight = 0;
  if (g_gxState.xfbHeight != 0 && g_gxState.dispCopySrc.height > 0) {
    const auto [logicalWidth, logicalHeight] = logical_fb_size();
    const auto [targetWidth, targetHeight] = gfx::get_render_target_size();
    const float scale = g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE || logicalHeight == 0 || targetHeight == 0
                            ? 1.f
                            : static_cast<float>(targetHeight) / static_cast<float>(logicalHeight);
    dstY = static_cast<s32>(static_cast<float>(g_gxState.dispCopyDstY) * scale + 0.5f);
    xfbHeight = static_cast<u32>(static_cast<float>(g_gxState.xfbHeight) * scale + 0.5f);
  }
  gfx::copy_disp(rect, clearColor, clearAlpha, clearDepth, g_gxState.clearColor, clear_depth_value(), dstY,
                 xfbHeight);
}

void clear_efb(const gfx::ClipRect& logicalRect, bool wholeTarget, bool clearColor, bool clearAlpha, bool clearDepth,
               GXColor color, u32 z) noexcept {
  gfx::ClipRect rect;
  if (wholeTarget) {
    const auto [width, height] = gfx::get_render_target_size();
    rect = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
  } else {
    rect = map_logical_scissor(logicalRect);
  }
  const Vec4 clearColorValue{
      static_cast<float>(color.r) / 255.f,
      static_cast<float>(color.g) / 255.f,
      static_cast<float>(color.b) / 255.f,
      static_cast<float>(color.a) / 255.f,
  };
  gfx::clear_rect(rect, clearColor, clearAlpha && efb_has_alpha(g_gxState.pixelFmt), clearDepth, clearColorValue,
                  clear_depth_value(z));
}
} // namespace aurora::gx

extern "C" {
GXRenderModeObj GXNtsc480IntDf = {
    VI_TVMODE_NTSC_INT,
    640,
    480,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {8, 8, 10, 12, 10, 8, 8},
};
GXRenderModeObj GXNtsc480Int = {
    VI_TVMODE_NTSC_INT,
    640,
    480,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {0, 0, 21, 22, 21, 0, 0},
};
GXRenderModeObj GXPal528IntDf = {
    VI_TVMODE_PAL_INT,
    704,
    528,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {8, 8, 10, 12, 10, 8, 8},
};
GXRenderModeObj GXMpal480IntDf = {
    VI_TVMODE_PAL_INT,
    640,
    480,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {8, 8, 10, 12, 10, 8, 8},
};

void GXAdjustForOverscan(GXRenderModeObj* rmin, GXRenderModeObj* rmout, u16 hor, u16 ver) {
  const GXRenderModeObj in = *rmin;
  const u16 hor2 = hor * 2;
  const u16 ver2 = ver * 2;
  const u32 efbHeight = in.efbHeight;

  *rmout = in;
  rmout->fbWidth = in.fbWidth - hor2;
  rmout->efbHeight = efbHeight - (ver2 * efbHeight) / in.xfbHeight;

  if (in.xFBmode == VI_XFBMODE_SF && (in.viTVmode & 2) != 2) {
    rmout->xfbHeight = in.xfbHeight - ver;
  } else {
    rmout->xfbHeight = in.xfbHeight - ver2;
  }

  rmout->viWidth = in.viWidth - hor2;
  rmout->viHeight = in.viHeight - ver2;
  rmout->viXOrigin = in.viXOrigin + hor;
  rmout->viYOrigin = in.viYOrigin + ver;
}

void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
  GX_WRITE_AURORA(GX_AURORA_LOAD_DISP_COPY_SRC);
  GX_WRITE_U32(left);
  GX_WRITE_U32(top);
  GX_WRITE_U32(wd);
  GX_WRITE_U32(ht);
}

void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
  GX_WRITE_AURORA(GX_AURORA_LOAD_COPY_SRC);
  GX_WRITE_U32(left);
  GX_WRITE_U32(top);
  GX_WRITE_U32(wd);
  GX_WRITE_U32(ht);
}

void GXSetDispCopyDst(u16 wd, u16 ht) {}

void GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap) {
  GX_WRITE_AURORA(GX_AURORA_LOAD_COPY_DST);
  GX_WRITE_U32(wd);
  GX_WRITE_U32(ht);
  GX_WRITE_U32(fmt);
  GX_WRITE_U8(mipmap != GX_FALSE);
}

void GXSetDispCopyFrame2Field(GXCopyMode mode) {}

void GXSetCopyClamp(GXFBClamp clamp) {}

u32 GXSetDispCopyYScale(f32 vscale) { return 0; }

void GXSetCopyClear(GXColor color, u32 depth) {
  // BP 0x4F: clear color R + A
  u32 reg0 = 0;
  SET_REG_FIELD(0, reg0, 8, 0, color.r);
  SET_REG_FIELD(0, reg0, 8, 8, color.a);
  SET_REG_FIELD(0, reg0, 8, 24, 0x4F);
  GX_WRITE_RAS_REG(reg0);

  // BP 0x50: clear color B + G
  u32 reg1 = 0;
  SET_REG_FIELD(0, reg1, 8, 0, color.b);
  SET_REG_FIELD(0, reg1, 8, 8, color.g);
  SET_REG_FIELD(0, reg1, 8, 24, 0x50);
  GX_WRITE_RAS_REG(reg1);

  // BP 0x51: clear Z (24-bit)
  u32 reg2 = 0;
  SET_REG_FIELD(0, reg2, 24, 0, depth);
  SET_REG_FIELD(0, reg2, 8, 24, 0x51);
  GX_WRITE_RAS_REG(reg2);
  __gx->bpSent = 1;
}

void GXSetCopyFilter(GXBool aa, const u8 sample_pattern[12][2], GXBool vf, const u8 vfilter[7]) {}

void GXSetDispCopyGamma(GXGamma gamma) {}

void GXCopyDisp(void* dest, GXBool clear) {
  u32 reg = 0;
  SET_REG_FIELD(0, reg, 1, 11, clear != GX_FALSE);
  SET_REG_FIELD(0, reg, 1, 14, 1);
  SET_REG_FIELD(0, reg, 8, 24, 0x52);
  GX_WRITE_RAS_REG(reg);
  aurora::gx::fifo::publish();
}

void GXCopyTex(void* dest, GXBool clear) {
  GX_WRITE_AURORA(GX_AURORA_LOAD_COPY_DEST);
  GX_WRITE_U64(reinterpret_cast<u64>(dest));

  SET_REG_FIELD(0, __gx->cpTex, 1, 11, clear != GX_FALSE);
  SET_REG_FIELD(0, __gx->cpTex, 1, 14, 0);
  SET_REG_FIELD(0, __gx->cpTex, 8, 24, 0x52);
  GX_WRITE_RAS_REG(__gx->cpTex);
  aurora::gx::fifo::publish();
}

static u32 get_num_xfb_lines(u32 height, u32 scale) {
  u32 actualHeight = (height - 1) * 0x100 / scale + 1;

  u32 newScale = scale;
  if (newScale > 0x80 && newScale < 0x100) {
    while (newScale % 2 == 0) {
      newScale /= 2;
    }
    if (height % newScale == 0) {
      actualHeight++;
    }
  }

  if (actualHeight > 0x400) {
    actualHeight = 0x400;
  }
  return actualHeight;
}

f32 GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight) {
  u32 height1 = xfbHeight;
  f32 scale1 = static_cast<f32>(xfbHeight) / static_cast<f32>(efbHeight);
  u32 height2 = get_num_xfb_lines(efbHeight, static_cast<u32>(256.0f / scale1) & 0x1FF);

  while (height2 > xfbHeight) {
    height1--;
    scale1 = static_cast<f32>(height1) / static_cast<f32>(efbHeight);
    height2 = get_num_xfb_lines(efbHeight, static_cast<u32>(256.0f / scale1) & 0x1FF);
  }

  f32 scale2 = scale1;
  while (height2 < xfbHeight) {
    scale2 = scale1;
    height1++;
    scale1 = static_cast<f32>(height1) / static_cast<f32>(efbHeight);
    height2 = get_num_xfb_lines(efbHeight, static_cast<u32>(256.0f / scale1) & 0x1FF);
  }

  return scale2;
}

u16 GXGetNumXfbLines(u16 efbHeight, f32 yScale) {
  return get_num_xfb_lines(efbHeight, static_cast<u32>(256.0f / yScale) & 0x1FF);
}
// TODO GXClearBoundingBox
// TODO GXReadBoundingBox
}
