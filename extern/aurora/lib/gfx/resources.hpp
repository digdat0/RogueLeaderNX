#pragma once

#include "types.hpp"

namespace aurora::gfx {
inline constexpr bool UseTextureBuffer = true;
// Per frame (upstream: 24 / 5 / 2 / 8 MiB). Rogue Leader's Battle of Hoth intro starts with three
// game frames in one presented frame (~54,000 draws): uniforms 29 MiB, vertices 6, indices 3.2,
// storage 13.3. A draw that still does not fit is dropped (frame_has_room), not fatal.
inline constexpr uint64_t UniformBufferSize = 50331648; // 48 MiB
inline constexpr uint64_t VertexBufferSize = 10485760;   // 10 MiB
inline constexpr uint64_t IndexBufferSize = 5242880;     // 5 MiB
inline constexpr uint64_t StorageBufferSize = 20971520; // 20 MiB
inline constexpr uint64_t TextureUploadSize = 25165824; // 24 MiB

namespace detail {
struct Resources {
  wgpu::Buffer vertexBuffer;
  wgpu::Buffer uniformBuffer;
  wgpu::Buffer indexBuffer;
  wgpu::Buffer storageBuffer;
  wgpu::BindGroupLayout staticBindGroupLayout;
  wgpu::BindGroup staticBindGroup;
  wgpu::BindGroupLayout uniformBindGroupLayout;
  wgpu::BindGroup uniformBindGroup;
  wgpu::Limits limits;
  AuroraStats stats{};
};

Resources& resources() noexcept;
} // namespace detail
} // namespace aurora::gfx
