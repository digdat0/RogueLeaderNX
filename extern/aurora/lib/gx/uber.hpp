#pragma once

#include "gx.hpp"
#include "pipeline.hpp"

namespace aurora::gx {
// Ubershaders (Dolphin's hybrid mode): one generic shader reads the TEV, texgen, lighting, vertex
// format, fog and alpha test state from its uniform block, so a draw whose specialized pipeline is
// not built yet is drawn with it instead of being skipped (no pop-in). What WebGPU fixes per
// pipeline (blend, depth, cull, line/point expansion, zfreeze output, destination alpha) still
// makes separate uber pipelines: their configs are PipelineConfigs with an empty shader config and
// UberMarker in the deprecated msaaSamples field, so they go through the pipeline cache (persisted,
// precompiled at startup) like the specialized ones.
enum class UberMode : u8 {
  Off,       // draws without a ready pipeline are skipped (upstream behaviour)
  Hybrid,    // the ubershader stands in until the specialized pipeline is ready
  Exclusive, // every draw uses the ubershader (testing)
};
extern UberMode g_uberMode;

inline constexpr uint32_t UberMarker = 0x52424555; // "UBER"

inline bool is_uber_config(const PipelineConfig& config) noexcept { return config.msaaSamples == UberMarker; }

// The uber pipeline config standing in for a specialized one: its fixed-function state only.
PipelineConfig uber_pipeline_config(const PipelineConfig& config) noexcept;

wgpu::ShaderModule build_uber_shader(const PipelineConfig& config, const gfx::RenderTargetLayout& layout,
                                     DstAlphaMode dstAlphaMode) noexcept;

// shader_info.cpp: the ubershader's uniform block for the current state.
gfx::Range build_uber_uniform(const ShaderConfig& config, const ShaderInfo& info) noexcept;
} // namespace aurora::gx
