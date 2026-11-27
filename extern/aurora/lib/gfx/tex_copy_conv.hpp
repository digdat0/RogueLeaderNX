#pragma once

#include "types.hpp"

#include <dolphin/gx/GXEnum.h>

#include <array>

namespace aurora::gfx::tex_copy_conv {

enum class SampleFilter : uint8_t {
  Nearest,
  Linear,
};

struct alignas(16) Uniforms {
  Vec2<float> offset;
  Vec2<float> scale{1.f, 1.f};
  uint32_t opaqueAlpha = 0;
  std::array<uint32_t, 3> _pad{};
};
static_assert(sizeof(Uniforms) == 32);

struct ConvRequest {
  GXTexFmt fmt;
  GXPixelFmt srcFmt;
  wgpu::TextureView srcView; // View of resolved EFB / offscreen color/depth
  Range uniformRange;        // Uniforms
  TextureHandle dst;         // Destination texture
  SampleFilter sampleFilter = SampleFilter::Nearest;
  ClipRect dstRect{};        // part of dst written (width 0: all of it, cleared first)
};

bool needs_conversion(GXTexFmt fmt);

void initialize();
void shutdown();
void run(const wgpu::CommandEncoder& cmd, const ConvRequest& req);
void blit(const wgpu::CommandEncoder& cmd, const ConvRequest& req);

bool snapshot_depth_supported() noexcept;
void snapshot_depth(const wgpu::CommandEncoder& cmd, const wgpu::TextureView& srcDepth, uint32_t msaaSamples,
                    const wgpu::TextureView& dst);
// A multisampled depth buffer's sample 0 in a single-sampled depth texture of that size (kept
// between calls), for depth texture copies from a multisampled EFB; null if unsupported.
wgpu::TextureView resolve_depth_ms(const wgpu::CommandEncoder& cmd, const wgpu::TextureView& srcDepth, uint32_t width,
                                   uint32_t height);

} // namespace aurora::gfx::tex_copy_conv
