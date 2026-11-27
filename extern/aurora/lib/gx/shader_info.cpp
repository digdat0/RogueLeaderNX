#include "shader_info.hpp"

#include "../gfx/recording.hpp"
#include "uber.hpp"

#include <atomic>
#include <cmath>
#include <cstddef>

#include <tracy/Tracy.hpp>

namespace aurora::gx {
namespace {
constexpr Module Log{"aurora::gx"};

bool is_alpha_bump_channel(GXChannelID id) { return id == GX_ALPHA_BUMP || id == GX_ALPHA_BUMPN; }

Light normalize_light_dir(Light light) {
  const double x = light.dir[0];
  const double y = light.dir[1];
  const double z = light.dir[2];
  const double len = std::sqrt(x * x + y * y + z * z);
  if (!std::isfinite(len)) {
    light.dir = {0.f, 0.f, 0.f};
  } else if (len > 0.0) {
    light.dir = {static_cast<float>(x / len), static_cast<float>(y / len), static_cast<float>(z / len)};
  }
  return light;
}

Vec4<float> texture_size_bias(const gfx::TextureBind& tex) {
  auto width = static_cast<float>(tex.texObj.width());
  auto height = static_cast<float>(tex.texObj.height());
  float vpBias = 0.f;
  if (tex.ref && tex.ref->hasArbitraryMips) {
    const float viewportScale =
        std::min(g_gxState.renderViewport.width / std::max(g_gxState.logicalViewport.width, 1.f),
                 g_gxState.renderViewport.height / std::max(g_gxState.logicalViewport.height, 1.f));
    const float replacementScale = static_cast<float>(tex.ref->size.width) / std::max(width, 1.f);
    vpBias = std::log2(viewportScale / std::max(replacementScale, 0.001f));
  }
  return {width, height, tex.texObj.lod_bias() + vpBias, 0.0f};
}

void color_arg_reg_info(GXTevColorArg arg, const TevStage& stage, ShaderInfo& info) {
  switch (arg) {
  case GX_CC_CPREV:
  case GX_CC_APREV:
    if (!info.writesTevReg.test(GX_TEVPREV)) {
      info.loadsTevReg.set(GX_TEVPREV);
    }
    break;
  case GX_CC_C0:
  case GX_CC_A0:
    if (!info.writesTevReg.test(GX_TEVREG0)) {
      info.loadsTevReg.set(GX_TEVREG0);
    }
    break;
  case GX_CC_C1:
  case GX_CC_A1:
    if (!info.writesTevReg.test(GX_TEVREG1)) {
      info.loadsTevReg.set(GX_TEVREG1);
    }
    break;
  case GX_CC_C2:
  case GX_CC_A2:
    if (!info.writesTevReg.test(GX_TEVREG2)) {
      info.loadsTevReg.set(GX_TEVREG2);
    }
    break;
  case GX_CC_TEXC:
  case GX_CC_TEXA:
    if (stage.texMapId != GX_TEXMAP_NULL) {
      CHECK(stage.texCoordId != GX_TEXCOORD_NULL, "tex coord not bound");
      info.sampledTexCoords.set(stage.texCoordId);
      info.sampledTextures.set(stage.texMapId);
    }
    break;
  case GX_CC_RASC:
  case GX_CC_RASA:
    if (stage.channelId != GX_COLOR_NULL && stage.channelId != GX_COLOR_ZERO &&
        !is_alpha_bump_channel(stage.channelId)) {
      info.sampledColorChannels.set(color_channel(stage.channelId));
    }
    break;
  case GX_CC_KONST:
    switch (stage.kcSel) {
    case GX_TEV_KCSEL_K0:
    case GX_TEV_KCSEL_K0_R:
    case GX_TEV_KCSEL_K0_G:
    case GX_TEV_KCSEL_K0_B:
    case GX_TEV_KCSEL_K0_A:
      info.sampledKColors.set(0);
      break;
    case GX_TEV_KCSEL_K1:
    case GX_TEV_KCSEL_K1_R:
    case GX_TEV_KCSEL_K1_G:
    case GX_TEV_KCSEL_K1_B:
    case GX_TEV_KCSEL_K1_A:
      info.sampledKColors.set(1);
      break;
    case GX_TEV_KCSEL_K2:
    case GX_TEV_KCSEL_K2_R:
    case GX_TEV_KCSEL_K2_G:
    case GX_TEV_KCSEL_K2_B:
    case GX_TEV_KCSEL_K2_A:
      info.sampledKColors.set(2);
      break;
    case GX_TEV_KCSEL_K3:
    case GX_TEV_KCSEL_K3_R:
    case GX_TEV_KCSEL_K3_G:
    case GX_TEV_KCSEL_K3_B:
    case GX_TEV_KCSEL_K3_A:
      info.sampledKColors.set(3);
      break;
    default:
      break;
    }
    break;
  default:
    break;
  }
}

void alpha_arg_reg_info(GXTevAlphaArg arg, const TevStage& stage, ShaderInfo& info) {
  switch (arg) {
  case GX_CA_APREV:
    if (!info.writesTevReg.test(GX_TEVPREV)) {
      info.loadsTevReg.set(GX_TEVPREV);
    }
    break;
  case GX_CA_A0:
    if (!info.writesTevReg.test(GX_TEVREG0)) {
      info.loadsTevReg.set(GX_TEVREG0);
    }
    break;
  case GX_CA_A1:
    if (!info.writesTevReg.test(GX_TEVREG1)) {
      info.loadsTevReg.set(GX_TEVREG1);
    }
    break;
  case GX_CA_A2:
    if (!info.writesTevReg.test(GX_TEVREG2)) {
      info.loadsTevReg.set(GX_TEVREG2);
    }
    break;
  case GX_CA_TEXA:
    if (stage.texMapId != GX_TEXMAP_NULL) {
      CHECK(stage.texCoordId != GX_TEXCOORD_NULL, "tex coord not bound");
      info.sampledTexCoords.set(stage.texCoordId);
      info.sampledTextures.set(stage.texMapId);
    }
    break;
  case GX_CA_RASA:
    if (stage.channelId != GX_COLOR_NULL && stage.channelId != GX_COLOR_ZERO &&
        !is_alpha_bump_channel(stage.channelId)) {
      info.sampledColorChannels.set(color_channel(stage.channelId));
    }
    break;
  case GX_CA_KONST:
    switch (stage.kaSel) {
    case GX_TEV_KASEL_K0_R:
    case GX_TEV_KASEL_K0_G:
    case GX_TEV_KASEL_K0_B:
    case GX_TEV_KASEL_K0_A:
      info.sampledKColors.set(0);
      break;
    case GX_TEV_KASEL_K1_R:
    case GX_TEV_KASEL_K1_G:
    case GX_TEV_KASEL_K1_B:
    case GX_TEV_KASEL_K1_A:
      info.sampledKColors.set(1);
      break;
    case GX_TEV_KASEL_K2_R:
    case GX_TEV_KASEL_K2_G:
    case GX_TEV_KASEL_K2_B:
    case GX_TEV_KASEL_K2_A:
      info.sampledKColors.set(2);
      break;
    case GX_TEV_KASEL_K3_R:
    case GX_TEV_KASEL_K3_G:
    case GX_TEV_KASEL_K3_B:
    case GX_TEV_KASEL_K3_A:
      info.sampledKColors.set(3);
      break;
    default:
      break;
    }
    break;
  default:
    break;
  }
}

f32 tex_offset(GXTexOffset offs) noexcept {
  switch (offs) {
  default:
  case GX_TO_ZERO:
    return 0.f;
  case GX_TO_SIXTEENTH:
    return 1.f / 16.f;
  case GX_TO_EIGHTH:
    return 1.f / 8.f;
  case GX_TO_FOURTH:
    return 1.f / 4.f;
  case GX_TO_HALF:
    return 1.f / 2.f;
  case GX_TO_ONE:
    return 1.f;
  }
}

u32 point_texcoord_mask() noexcept {
  u32 mask = 0;
  for (int i = 0; i < MaxTexCoord; ++i) {
    if (g_gxState.texCoordScales[i].pointOffset) {
      mask |= 1 << i;
    }
  }
  return mask;
}

u32 line_texcoord_mask() noexcept {
  u32 mask = 0;
  for (int i = 0; i < MaxTexCoord; ++i) {
    if (g_gxState.texCoordScales[i].lineOffset) {
      mask |= 1 << i;
    }
  }
  return mask;
}
} // namespace

ShaderInfo build_shader_info(const ShaderConfig& config) noexcept {
  ZoneScoped;

  ShaderInfo info{
      // render/logical viewport size, proj, depth_range
      .uniformSize = 8 + 8 + 64 + 16,
  };
  if (config.zFreeze) {
    info.zFreeze = true;
    info.uniformSize += 16; // zfreeze_plane
  }

  if (config.lineMode != 0) {
    info.uniformSize += 4 + 4 + 4 + 4; // line_width, line_aspect_y, line_tex_offset, line_texcoord_mask
    info.lineMode = config.lineMode;
  }

  for (int attr = 0; attr < config.attrs.size(); attr++) {
    const auto attrType = config.attrs[attr].attrType;
    if ((attr == GX_VA_PNMTXIDX || (attr >= GX_VA_TEX0MTXIDX && attr <= GX_VA_TEX7MTXIDX)) && attrType == GX_DIRECT) {
      info.indexAttr.set(attr);
    } else if (attrType == GX_INDEX8 || attrType == GX_INDEX16) {
      info.indexAttr.set(attr);
    }
  }

  // 10 position matrices, 10 texture matrices, the GX_IDENTITY row, 10 normal matrices.
  info.uniformSize += sizeof(Mat3x4<float>) * 31;

  for (int i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    // Color pass
    color_arg_reg_info(stage.colorPass.a, stage, info);
    color_arg_reg_info(stage.colorPass.b, stage, info);
    color_arg_reg_info(stage.colorPass.c, stage, info);
    color_arg_reg_info(stage.colorPass.d, stage, info);
    info.writesTevReg.set(stage.colorOp.outReg);

    // Alpha pass
    alpha_arg_reg_info(stage.alphaPass.a, stage, info);
    alpha_arg_reg_info(stage.alphaPass.b, stage, info);
    alpha_arg_reg_info(stage.alphaPass.c, stage, info);
    alpha_arg_reg_info(stage.alphaPass.d, stage, info);
    if (!info.writesTevReg.test(stage.alphaOp.outReg)) {
      // If we're writing alpha to a register that's not been
      // written to in the shader, load from uniform buffer
      info.loadsTevReg.set(stage.alphaOp.outReg);
      info.writesTevReg.set(stage.alphaOp.outReg);
    }
  }
  for (int i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    // Skip if not enabled
    if (stage.indTexStage >= config.numIndStages) {
      continue;
    }
    const bool usesIndStage = stage.indTexMtxId != GX_ITM_OFF || is_alpha_bump_channel(stage.channelId);
    const bool usesTevTexCoord = stage.indTexMtxId != GX_ITM_OFF || stage.indTexWrapS != GX_ITW_OFF ||
                                 stage.indTexWrapT != GX_ITW_OFF || stage.indTexAddPrev;
    if (usesTevTexCoord && stage.texCoordId != GX_TEXCOORD_NULL) {
      info.sampledTexCoords.set(stage.texCoordId);
    }
    if (usesTevTexCoord && stage.texMapId != GX_TEXMAP_NULL &&
        (stage.indTexMtxId != GX_ITM_OFF || stage.indTexAddPrev)) {
      info.sampledTextures.set(stage.texMapId);
    }
    if (!usesIndStage) {
      continue;
    }
    info.usedIndStages.set(stage.indTexStage);
    const auto& indStage = config.indStages[stage.indTexStage];
    info.sampledTextures.set(indStage.texMapId);
    info.sampledTexCoords.set(indStage.texCoordId);
    info.sampledIndTextures.set(indStage.texMapId);
    // Track which indirect matrix is used
    if (stage.indTexMtxId >= GX_ITM_0 && stage.indTexMtxId <= GX_ITM_2) {
      info.usedIndTexMtxs.set(stage.indTexMtxId - GX_ITM_0);
    } else if (stage.indTexMtxId >= GX_ITM_S0 && stage.indTexMtxId <= GX_ITM_S2) {
      info.usedIndTexMtxs.set(stage.indTexMtxId - GX_ITM_S0);
    } else if (stage.indTexMtxId >= GX_ITM_T0 && stage.indTexMtxId <= GX_ITM_T2) {
      info.usedIndTexMtxs.set(stage.indTexMtxId - GX_ITM_T0);
    }
  }

  // Emboss bump needs its source texcoord generated and a light enabled
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (!info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = config.tcgs[i];
    if (tcg.type >= GX_TG_BUMP0 && tcg.type <= GX_TG_BUMP7) {
      info.sampledTexCoords.set(tcg.embossSrc);
      info.lightingEnabled = true;
    }
  }

  // Binormal / tangent wanted (emboss, or a texgen from them) by a vertex format without NBT
  // normals: the ones the vertex unit last held, from the uniform (GXState::nbtCache)
  {
    bool needsNbt = false;
    for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
      const auto& tcg = config.tcgs[i];
      needsNbt = needsNbt || (info.sampledTexCoords.test(i) &&
                              ((tcg.type >= GX_TG_BUMP0 && tcg.type <= GX_TG_BUMP7) || tcg.src == GX_TG_BINRM ||
                               tcg.src == GX_TG_TANGENT));
    }
    const auto& nrm = config.attrs[GX_VA_NRM];
    if (needsNbt && (nrm.attrType == GX_NONE || nrm.cnt != 9)) {
      info.nbtCache = true;
      info.uniformSize += 2 * sizeof(Vec4<float>); // nbt_cache
    }
  }

  // Texgens can consume a color channel even when TEV never reads it directly.
  // Include these channels before calculating lighting and uniform requirements.
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (!info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = config.tcgs[i];
    if (tcg.type >= GX_TG_BUMP0 && tcg.type <= GX_TG_BUMP7) {
      continue;
    }
    if (tcg.src == GX_TG_COLOR0) {
      info.sampledColorChannels.set(0);
    } else if (tcg.src == GX_TG_COLOR1) {
      info.sampledColorChannels.set(1);
    }
  }

  info.uniformSize += info.loadsTevReg.count() * sizeof(Vec4<float>);
  for (int i = 0; i < info.sampledColorChannels.size(); ++i) {
    if (info.sampledColorChannels.test(i)) {
      const auto& cc = config.colorChannels[i];
      const auto& cca = config.colorChannels[i + GX_ALPHA0];
      if (cc.lightingEnabled || cca.lightingEnabled) {
        info.lightingEnabled = true;
      }
    }
  }
  if (info.lightingEnabled) {
    // Lights + light state for all channels
    info.uniformSize += 16 + sizeof(Light) * GX::MaxLights;
  }
  for (int i = 0; i < info.sampledColorChannels.size(); ++i) {
    if (info.sampledColorChannels.test(i)) {
      const auto& cc = config.colorChannels[i];
      if (cc.lightingEnabled && cc.ambSrc == GX_SRC_REG) {
        info.uniformSize += sizeof(Vec4<float>);
      }
      if (cc.matSrc == GX_SRC_REG) {
        info.uniformSize += sizeof(Vec4<float>);
      }
      const auto& cca = config.colorChannels[i + GX_ALPHA0];
      if (cca.lightingEnabled && cca.ambSrc == GX_SRC_REG) {
        info.uniformSize += sizeof(Vec4<float>);
      }
      if (cca.matSrc == GX_SRC_REG) {
        info.uniformSize += sizeof(Vec4<float>);
      }
    }
  }
  info.uniformSize += info.sampledKColors.count() * sizeof(Vec4<float>);
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (!info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = config.tcgs[i];
    if (tcg.postMtx != GX_PTIDENTITY) {
      u32 postMtxIdx = (tcg.postMtx - GX_PTTEXMTX0) / 3;
      info.usesPTTexMtx.set(postMtxIdx);
    }
  }
  if (info.usesPTTexMtx.any()) {
    info.uniformSize += sizeof(Mat3x4<float>) * MaxPTTexMtx;
  }
  if (config.fogType != GX_FOG_NONE) {
    info.usesFog = true;
    info.uniformSize += sizeof(Fog);
  }
  info.uniformSize += MaxTexCoord * sizeof(Vec4<float>);
  if (info.usedIndTexMtxs.any()) {
    info.uniformSize += MaxIndTexMtxs * sizeof(Mat2x4<float>);
  }
  info.uniformSize += info.sampledTextures.count() * sizeof(Vec4<float>);
  info.uniformSize = gfx::align_uniform(info.uniformSize);
  if (info.uniformSize > MaxUniformSize) {
    Log.fatal("Uniform size exceeds maximum: {} > {}", info.uniformSize, MaxUniformSize);
  }
  return info;
}

static bool stage_samples_texture(const TevStage& stage) noexcept {
  if (stage.texMapId == GX_TEXMAP_NULL) {
    return false;
  }
  const auto isTexColor = [](GXTevColorArg arg) { return arg == GX_CC_TEXC || arg == GX_CC_TEXA; };
  const auto& c = stage.colorPass;
  const auto& a = stage.alphaPass;
  return isTexColor(c.a) || isTexColor(c.b) || isTexColor(c.c) || isTexColor(c.d) || a.a == GX_CA_TEXA ||
         a.b == GX_CA_TEXA || a.c == GX_CA_TEXA || a.d == GX_CA_TEXA;
}

static std::string describe_texturing(const ShaderConfig& config, u32 numTexGens) {
  std::string out = fmt::format("numTexGens {} numTevStages {} numIndStages {}", numTexGens, config.tevStageCount,
                                config.numIndStages);
  for (u32 i = 0; i < config.tevStageCount; ++i) {
    const auto& st = config.tevStages[i];
    out += fmt::format("\n  tev{}: texCoord {} texMap {} channel {} color {} {} {} {} alpha {} {} {} {} | indStage {} "
                       "indMtx {} wrap {} {} addPrev {}",
                       i, underlying(st.texCoordId), underlying(st.texMapId), underlying(st.channelId),
                       underlying(st.colorPass.a), underlying(st.colorPass.b), underlying(st.colorPass.c),
                       underlying(st.colorPass.d), underlying(st.alphaPass.a), underlying(st.alphaPass.b),
                       underlying(st.alphaPass.c), underlying(st.alphaPass.d), underlying(st.indTexStage),
                       underlying(st.indTexMtxId), underlying(st.indTexWrapS), underlying(st.indTexWrapT),
                       st.indTexAddPrev);
  }
  for (u32 i = 0; i < config.numIndStages; ++i) {
    const auto& ind = config.indStages[i];
    out += fmt::format("\n  ind{}: texCoord {} texMap {}", i, underlying(ind.texCoordId), underlying(ind.texMapId));
  }
  return out;
}

void validate_shader_config(const ShaderConfig& config, const ShaderInfo& info, u32 numTexGens) noexcept {
  for (u32 i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (!info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = config.tcgs[i];
    if (tcg.src == GX_MAX_TEXGENSRC && !(tcg.type >= GX_TG_BUMP0 && tcg.type <= GX_TG_BUMP7)) {
      UNLIKELY FATAL("GX: a stage reads texcoord {}, but no texgen makes it (undefined on console). Usually a "
                     "TEV or indirect stage is still set from an earlier draw.\n{}",
                     i, describe_texturing(config, numTexGens));
    }
    if (tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7) {
      const auto attr = static_cast<GXAttr>(GX_VA_TEX0 + (tcg.src - GX_TG_TEX0));
      if (config.attrs[attr].attrType == GX_NONE) {
        UNLIKELY FATAL("GX: texgen {} reads vertex attribute TEX{}, but the vertex descriptor does not have it "
                       "(undefined on console). Usually the texgen or the TEV stage is still set from an "
                       "earlier draw.\n{}",
                       i, static_cast<int>(tcg.src - GX_TG_TEX0), describe_texturing(config, numTexGens));
      }
    }
  }
  for (u32 i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    if (stage.indTexMtxId != GX_ITM_OFF && stage.indTexStage >= config.numIndStages &&
        stage_samples_texture(stage)) {
      static std::atomic<u32> sReported{0};
      if (sReported.fetch_add(1, std::memory_order_relaxed) < 8) {
        Log.warn("GX: TEV stage {} uses indirect stage {}, but only {} indirect stages are enabled (undefined on "
                 "console, drawn without the indirect offset). Usually the TEV stage is still set from an earlier "
                 "draw.\n{}",
                 i, underlying(stage.indTexStage), config.numIndStages, describe_texturing(config, numTexGens));
      }
    }
  }
}

static Mat4x4<float> draw_projection() noexcept {
  auto proj = g_gxState.proj;
  if constexpr (UseReversedZ) {
    proj.m2 = proj.m2 * Vec4{-1.f, -1.f, -1.f, -1.f};
  } else {
    proj.m2 = proj.m2 + proj.m3;
  }
  apply_widescreen_scale(proj);
  return proj;
}

// The viewport's depth range is applied in the vertex shader (the render pass viewport always
// spans 0-1): games may use ranges WebGPU cannot express, e.g. far = 10, where hardware
// saturates the 24-bit depth.
static Vec4<f32> draw_depth_range() noexcept {
  const auto& vp = g_gxState.logicalViewport;
  const f32 range = vp.zfar - vp.znear;
  return Vec4<f32>{UseReversedZ ? 1.f - vp.zfar : vp.znear, range, 0.f, 0.f};
}

static Fog draw_fog() noexcept {
  const auto& state = g_gxState.fog;
  const float logicalWidth = std::max(g_gxState.logicalViewport.width, 1.f);
  const float renderWidth = std::max(g_gxState.renderViewport.width, 1.f);
  Fog fog{
      .color = state.color,
      .a = state.a,
      .b = state.b,
      .c = state.c,
      .rangeCenter = ((static_cast<float>(state.rangeCenter) - g_gxState.logicalViewport.left) / logicalWidth) * 2.f -
                     1.f + (g_gxState.renderViewport.left / renderWidth) * 2.f,
  };
  for (u32 i = 0; i < state.rangeK.size(); ++i) {
    const u32 source = (i & ~1u) | (1u - (i & 1u));
    fog.rangeK[i / 4][i % 4] = static_cast<float>(state.rangeK[source]) / 64.f;
  }
  fog.rangeK[2][2] = fog.rangeK[2][1];
  fog.rangeK[2][3] = fog.rangeK[2][1];
  return fog;
}

static void fill_uniform(ByteBuffer& buf, const ShaderInfo& info) noexcept {
  buf.reserve_extra(info.uniformSize);

  buf.append<f32>(g_gxState.renderViewport.width);
  buf.append<f32>(g_gxState.renderViewport.height);
  buf.append<f32>(g_gxState.logicalViewport.width);
  buf.append<f32>(g_gxState.logicalViewport.height);
  if (info.lineMode != 0) {
    if (info.lineMode == 3) { // GX_POINTS
      buf.append<f32>(static_cast<f32>(g_gxState.pointSize) / 6.f);
      buf.append<f32>(1.0f);
      buf.append<f32>(tex_offset(g_gxState.pointTexOffset));
      buf.append<u32>(point_texcoord_mask());
    } else { // GX_LINES / GX_LINESTRIP
      buf.append<f32>(static_cast<f32>(g_gxState.lineWidth) / 6.f);
      buf.append<f32>(g_gxState.lineHalfAspect ? 0.5f : 1.f);
      buf.append<f32>(tex_offset(g_gxState.lineTexOffset));
      buf.append<u32>(line_texcoord_mask());
    }
  }
  buf.append(draw_projection());
  buf.append(draw_depth_range());
  if (info.zFreeze) {
    buf.append(g_gxState.zFreezePlane);
  }
  if (info.nbtCache) {
    buf.append(g_gxState.nbtCache[0]);
    buf.append(g_gxState.nbtCache[1]);
  }

  for (int i = 0; i < MaxPnMtx; i++) {
    buf.append(g_gxState.pnMtx[i].pos);
  }

  for (int i = 0; i < MaxTexMtx; i++) {
    buf.append(g_gxState.texMtxs[i]);
  }
  // XF row 60 (GX_IDENTITY) used as a position matrix
  buf.append(Mat3x4<float>{{1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}});

  for (int i = 0; i < MaxPnMtx; i++) {
    buf.append(g_gxState.pnMtx[i].nrm);
  }

  for (int i = 0; i < info.loadsTevReg.size(); ++i) {
    if (info.loadsTevReg.test(i)) {
      buf.append(g_gxState.colorRegs[i]);
    }
  }
  if (info.lightingEnabled) {
    // Lights
    static_assert(sizeof(g_gxState.lights) == 80 * GX::MaxLights);
    for (const auto& light : g_gxState.lights) {
      buf.append(normalize_light_dir(light));
    }
    // Light state for all channels
    for (int i = 0; i < 4; ++i) {
      buf.append<u32>(g_gxState.colorChannelState[i].lightMask.to_ulong());
    }
  }
  for (int i = 0; i < info.sampledColorChannels.size(); ++i) {
    if (!info.sampledColorChannels.test(i)) {
      continue;
    }
    const auto& ccc = g_gxState.colorChannelConfig[i];
    const auto& ccs = g_gxState.colorChannelState[i];
    if (ccc.lightingEnabled && ccc.ambSrc == GX_SRC_REG) {
      buf.append(ccs.ambColor);
    }
    if (ccc.matSrc == GX_SRC_REG) {
      buf.append(ccs.matColor);
    }
    const auto& ccca = g_gxState.colorChannelConfig[i + GX_ALPHA0];
    const auto& ccsa = g_gxState.colorChannelState[i + GX_ALPHA0];
    if (ccca.lightingEnabled && ccca.ambSrc == GX_SRC_REG) {
      buf.append(ccsa.ambColor);
    }
    if (ccca.matSrc == GX_SRC_REG) {
      buf.append(ccsa.matColor);
    }
  }
  for (int i = 0; i < info.sampledKColors.size(); ++i) {
    if (info.sampledKColors.test(i)) {
      buf.append(g_gxState.kcolors[i]);
    }
  }
  if (info.usesPTTexMtx.any()) {
    for (int i = 0; i < info.usesPTTexMtx.size(); ++i) {
      buf.append(g_gxState.ptTexMtxs[i]);
    }
  }
  if (info.usesFog) {
    buf.append(draw_fog());
  }
  for (const auto& scale : g_gxState.texCoordScales) {
    buf.append(Vec4{static_cast<f32>(scale.scaleS) + 1.0f, static_cast<f32>(scale.scaleT) + 1.0f, 0.0f, 0.0f});
  }
  if (info.usedIndTexMtxs.any()) {
    for (u32 i = 0; i < MaxIndTexMtxs; ++i) {
      const auto& mtx = g_gxState.indTexMtxs[i];
      buf.append(Vec4{mtx.mtx.m0.x, mtx.mtx.m0.y, mtx.mtx.m1.x, mtx.mtx.m1.y});
      buf.append(Vec4{mtx.mtx.m2.x, mtx.mtx.m2.y, std::exp2f(mtx.scaleExp), 0.0f});
    }
  }
  for (int i = 0; i < info.sampledTextures.size(); ++i) {
    if (!info.sampledTextures.test(i)) {
      continue;
    }
    buf.append(texture_size_bias(get_texture(static_cast<GXTexMapID>(i))));
  }
}

gfx::Range build_uniform(const ShaderInfo& info) noexcept {
  ZoneScoped;
  static ByteBuffer buf;
  buf.clear();
  fill_uniform(buf, info);
  return gfx::push_uniform(buf.data(), buf.size());
}

namespace {
// The ubershader's uniform block: the Uniform struct in uber.cpp, byte for byte.
struct UberUniform {
  f32 renderViewport[2];
  f32 logicalViewport[2];
  f32 line[3]; // line width / point size, line aspect, texcoord offset
  u32 lineMask;
  Mat4x4<float> proj;
  Vec4<f32> depthRange;
  Vec4<f32> zfreezePlane;
  Mat3x4<float> postexMtx[MaxPnMtx + MaxTexMtx + 1];
  Mat3x4<float> nrmMtx[MaxPnMtx];
  Mat3x4<float> tcgPost[MaxTexCoord];
  Light lights[GX::MaxLights];
  Vec4<f32> tevRegs[MaxTevRegs];
  Vec4<f32> kcolors[MaxKColors];
  Vec4<f32> chanAmb[MaxColorChannels];
  Vec4<f32> chanMat[MaxColorChannels];
  Fog fog;
  Vec4<f32> texcoordScale[MaxTexCoord];
  Vec4<f32> indMtx[MaxIndTexMtxs * 2];
  Vec4<f32> texSizeBias[MaxTextures];
  u32 stages[MaxTevStages][4];
  u32 tcgs[MaxTexCoord][4];
  u32 attrs[24];
  u32 chans[4];
  u32 indStages[4];
  u32 counts[4];
  u32 flags[4];
  Vec4<f32> nbtCache[2]; // GXState::nbtCache
};
static_assert(offsetof(UberUniform, proj) == 32);
static_assert(offsetof(UberUniform, postexMtx) == 128);
static_assert(offsetof(UberUniform, lights) == 2000);
static_assert(offsetof(UberUniform, fog) == 2896);
static_assert(offsetof(UberUniform, stages) == 3328);
static_assert(offsetof(UberUniform, flags) == 3856);
static_assert(offsetof(UberUniform, nbtCache) == 3872);
static_assert(sizeof(UberUniform) == 3904);
static_assert(sizeof(UberUniform) <= MaxUniformSize);

// The specialized shaders wrap TEV inputs that may hold values outside 0-1 (a register whose last
// write was not clamped) like the hardware's 8 bits; these follow which registers are clamped.
bool uber_color_arg_normalized(GXTevColorArg arg, const std::array<bool, MaxTevRegs>& color,
                               const std::array<bool, MaxTevRegs>& alpha) noexcept {
  switch (arg) {
  case GX_CC_CPREV:
    return color[GX_TEVPREV];
  case GX_CC_APREV:
    return alpha[GX_TEVPREV];
  case GX_CC_C0:
    return color[GX_TEVREG0];
  case GX_CC_A0:
    return alpha[GX_TEVREG0];
  case GX_CC_C1:
    return color[GX_TEVREG1];
  case GX_CC_A1:
    return alpha[GX_TEVREG1];
  case GX_CC_C2:
    return color[GX_TEVREG2];
  case GX_CC_A2:
    return alpha[GX_TEVREG2];
  default:
    return true;
  }
}

bool uber_alpha_arg_normalized(GXTevAlphaArg arg, const std::array<bool, MaxTevRegs>& alpha) noexcept {
  switch (arg) {
  case GX_CA_APREV:
    return alpha[GX_TEVPREV];
  case GX_CA_A0:
    return alpha[GX_TEVREG0];
  case GX_CA_A1:
    return alpha[GX_TEVREG1];
  case GX_CA_A2:
    return alpha[GX_TEVREG2];
  default:
    return true;
  }
}

bool uber_stage_samples(const TevStage& stage) noexcept {
  if (stage.texMapId == GX_TEXMAP_NULL) {
    return false;
  }
  const auto& c = stage.colorPass;
  const auto& a = stage.alphaPass;
  return c.a == GX_CC_TEXC || c.a == GX_CC_TEXA || c.b == GX_CC_TEXC || c.b == GX_CC_TEXA || c.c == GX_CC_TEXC ||
         c.c == GX_CC_TEXA || c.d == GX_CC_TEXC || c.d == GX_CC_TEXA || a.a == GX_CA_TEXA || a.b == GX_CA_TEXA ||
         a.c == GX_CA_TEXA || a.d == GX_CA_TEXA;
}

u32 uber_tev_op(const TevOp& o) noexcept {
  return (static_cast<u32>(o.op) & 15) | (static_cast<u32>(o.bias) & 3) << 4 | (static_cast<u32>(o.scale) & 3) << 6 |
         static_cast<u32>(o.clamp) << 8 | (static_cast<u32>(o.outReg) & 3) << 9;
}
} // namespace

gfx::Range build_uber_uniform(const ShaderConfig& config, const ShaderInfo& info) noexcept {
  ZoneScoped;
  static UberUniform u;
  u = {};
  u.renderViewport[0] = g_gxState.renderViewport.width;
  u.renderViewport[1] = g_gxState.renderViewport.height;
  u.logicalViewport[0] = g_gxState.logicalViewport.width;
  u.logicalViewport[1] = g_gxState.logicalViewport.height;
  if (config.lineMode == 3) { // GX_POINTS
    u.line[0] = static_cast<f32>(g_gxState.pointSize) / 6.f;
    u.line[1] = 1.f;
    u.line[2] = tex_offset(g_gxState.pointTexOffset);
    u.lineMask = point_texcoord_mask();
  } else if (config.lineMode != 0) { // GX_LINES / GX_LINESTRIP
    u.line[0] = static_cast<f32>(g_gxState.lineWidth) / 6.f;
    u.line[1] = g_gxState.lineHalfAspect ? 0.5f : 1.f;
    u.line[2] = tex_offset(g_gxState.lineTexOffset);
    u.lineMask = line_texcoord_mask();
  }
  u.proj = draw_projection();
  u.depthRange = draw_depth_range();
  u.zfreezePlane = g_gxState.zFreezePlane;
  for (u32 i = 0; i < MaxPnMtx; ++i) {
    u.postexMtx[i] = g_gxState.pnMtx[i].pos;
    u.nrmMtx[i] = g_gxState.pnMtx[i].nrm;
  }
  for (u32 i = 0; i < MaxTexMtx; ++i) {
    u.postexMtx[MaxPnMtx + i] = g_gxState.texMtxs[i];
  }
  u.postexMtx[MaxPnMtx + MaxTexMtx] = Mat3x4<float>{{1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}};
  for (u32 i = 0; i < GX::MaxLights; ++i) {
    u.lights[i] = normalize_light_dir(g_gxState.lights[i]);
  }
  for (u32 i = 0; i < MaxTevRegs; ++i) {
    u.tevRegs[i] = g_gxState.colorRegs[i];
  }
  for (u32 i = 0; i < MaxKColors; ++i) {
    u.kcolors[i] = g_gxState.kcolors[i];
  }
  for (u32 c = 0; c < MaxColorChannels; ++c) {
    const auto& cc = config.colorChannels[c];
    const auto& state = g_gxState.colorChannelState[c];
    u.chanAmb[c] = state.ambColor;
    u.chanMat[c] = state.matColor;
    u.chans[c] = static_cast<u32>(cc.lightingEnabled) | static_cast<u32>(cc.matSrc == GX_SRC_VTX) << 1 |
                 static_cast<u32>(cc.ambSrc == GX_SRC_VTX) << 2 | (static_cast<u32>(cc.diffFn) & 3) << 3 |
                 (static_cast<u32>(cc.attnFn) & 3) << 5 | (static_cast<u32>(state.lightMask.to_ulong()) & 0xFF) << 8;
  }
  u.fog = draw_fog();
  for (u32 i = 0; i < MaxTexCoord; ++i) {
    const auto& scale = g_gxState.texCoordScales[i];
    u.texcoordScale[i] = Vec4{static_cast<f32>(scale.scaleS) + 1.0f, static_cast<f32>(scale.scaleT) + 1.0f, 0.0f, 0.0f};
  }
  for (u32 i = 0; i < MaxIndTexMtxs; ++i) {
    const auto& mtx = g_gxState.indTexMtxs[i];
    u.indMtx[i * 2] = Vec4{mtx.mtx.m0.x, mtx.mtx.m0.y, mtx.mtx.m1.x, mtx.mtx.m1.y};
    u.indMtx[i * 2 + 1] = Vec4{mtx.mtx.m2.x, mtx.mtx.m2.y, std::exp2f(mtx.scaleExp), 0.0f};
  }
  for (u32 i = 0; i < MaxTextures; ++i) {
    if (info.sampledTextures.test(i)) {
      u.texSizeBias[i] = texture_size_bias(get_texture(static_cast<GXTexMapID>(i)));
    }
  }

  // TEV stages (packing: see uber.cpp)
  std::array<bool, MaxTevRegs> colorNormalized{};
  std::array<bool, MaxTevRegs> alphaNormalized{};
  const u32 stageCount = std::min<u32>(config.tevStageCount, MaxTevStages);
  for (u32 i = 0; i < stageCount; ++i) {
    const auto& st = config.tevStages[i];
    const auto& c = st.colorPass;
    const auto& a = st.alphaPass;
    u32* w = u.stages[i];
    w[0] = static_cast<u32>(c.a) | static_cast<u32>(c.b) << 4 | static_cast<u32>(c.c) << 8 |
           static_cast<u32>(c.d) << 12 | static_cast<u32>(a.a) << 16 | static_cast<u32>(a.b) << 19 |
           static_cast<u32>(a.c) << 22 | static_cast<u32>(a.d) << 25;
    w[1] = uber_tev_op(st.colorOp);
    w[1] |= static_cast<u32>(!uber_color_arg_normalized(c.a, colorNormalized, alphaNormalized)) << 11;
    w[1] |= static_cast<u32>(!uber_color_arg_normalized(c.b, colorNormalized, alphaNormalized)) << 12;
    w[1] |= static_cast<u32>(!uber_color_arg_normalized(c.c, colorNormalized, alphaNormalized)) << 13;
    colorNormalized[st.colorOp.outReg & 3] = st.colorOp.clamp;
    w[1] |= uber_tev_op(st.alphaOp) << 16;
    w[1] |= static_cast<u32>(!uber_alpha_arg_normalized(a.a, alphaNormalized)) << 27;
    w[1] |= static_cast<u32>(!uber_alpha_arg_normalized(a.b, alphaNormalized)) << 28;
    w[1] |= static_cast<u32>(!uber_alpha_arg_normalized(a.c, alphaNormalized)) << 29;
    alphaNormalized[st.alphaOp.outReg & 3] = st.alphaOp.clamp;

    const u32 texMap = st.texMapId == GX_TEXMAP_NULL ? 15u : static_cast<u32>(st.texMapId) & 7;
    const u32 texCoord = st.texCoordId == GX_TEXCOORD_NULL ? 15u : static_cast<u32>(st.texCoordId) & 7;
    u32 ras = 2; // zero
    if (st.channelId == GX_ALPHA_BUMP) {
      ras = 3;
    } else if (st.channelId == GX_ALPHA_BUMPN) {
      ras = 4;
    } else if (st.channelId != GX_COLOR_ZERO && st.channelId != GX_COLOR_NULL) {
      ras = color_channel(st.channelId);
    }
    w[2] = (static_cast<u32>(st.kcSel) & 31) | (static_cast<u32>(st.kaSel) & 31) << 5 | texMap << 10 |
           texCoord << 14 | (static_cast<u32>(st.tevSwapTex) & 3) << 18 | (static_cast<u32>(st.tevSwapRas) & 3) << 20 |
           ras << 22 | static_cast<u32>(uber_stage_samples(st)) << 25;

    const bool hasIndStage = st.indTexStage < config.numIndStages;
    const bool needsIndCoord = st.indTexMtxId != GX_ITM_OFF;
    const bool needsTevTexCoord =
        needsIndCoord || st.indTexWrapS != GX_ITW_OFF || st.indTexWrapT != GX_ITW_OFF || st.indTexAddPrev;
    const bool hasBaseTexture = st.texMapId != GX_TEXMAP_NULL;
    const bool hasBaseCoord = st.texCoordId != GX_TEXCOORD_NULL && hasBaseTexture;
    u32 kind = 0;
    u32 mtx = 0;
    if (needsIndCoord && hasIndStage) {
      if (st.indTexMtxId >= GX_ITM_0 && st.indTexMtxId <= GX_ITM_2) {
        kind = 1;
        mtx = st.indTexMtxId - GX_ITM_0;
      } else if (st.indTexMtxId >= GX_ITM_S0 && st.indTexMtxId <= GX_ITM_S2 && hasBaseCoord) {
        kind = 2;
        mtx = st.indTexMtxId - GX_ITM_S0;
      } else if (st.indTexMtxId >= GX_ITM_T0 && st.indTexMtxId <= GX_ITM_T2 && hasBaseCoord) {
        kind = 3;
        mtx = st.indTexMtxId - GX_ITM_T0;
      }
    }
    const bool simple = st.indTexMtxId == GX_ITM_OFF && !st.indTexAddPrev;
    const bool bump = hasIndStage && st.indTexAlphaSel != GX_ITBA_OFF;
    w[3] = (static_cast<u32>(st.indTexStage) & 3) | (static_cast<u32>(st.indTexFormat) & 3) << 2 |
           (static_cast<u32>(st.indTexBiasSel) & 7) << 4 | kind << 9 | mtx << 11 |
           (static_cast<u32>(st.indTexWrapS) & 7) << 13 | (static_cast<u32>(st.indTexWrapT) & 7) << 16 |
           static_cast<u32>(st.indTexAddPrev) << 19 | static_cast<u32>(needsTevTexCoord) << 20 |
           static_cast<u32>(hasBaseCoord) << 22 | static_cast<u32>(simple) << 23 |
           static_cast<u32>(hasBaseTexture) << 24 | static_cast<u32>(bump) << 25 |
           (bump ? (static_cast<u32>(st.indTexAlphaSel) - 1) & 3 : 0u) << 26;
  }

  // texgens
  for (u32 i = 0; i < MaxTexCoord; ++i) {
    const auto& t = config.tcgs[i];
    u32 kind = 3; // SRTG
    if (t.type == GX_TG_MTX3x4) {
      kind = 0;
    } else if (t.type == GX_TG_MTX2x4) {
      kind = 1;
    } else if (t.type >= GX_TG_BUMP0 && t.type <= GX_TG_BUMP7) {
      kind = 2;
    }
    u32 matrix = 0; // identity
    if (info.indexAttr.test(GX_VA_TEX0MTXIDX + i)) {
      matrix = 2; // per vertex
    } else if (t.mtx != GX_IDENTITY) {
      matrix = 1 | (static_cast<u32>(t.mtx) / 3) << 4;
    }
    const bool post = t.postMtx != GX_PTIDENTITY;
    if (post) {
      u.tcgPost[i] = g_gxState.ptTexMtxs[((t.postMtx - GX_PTTEXMTX0) / 3) % MaxPTTexMtx];
    }
    u.tcgs[i][0] = kind | (kind == 2 ? static_cast<u32>(t.type - GX_TG_BUMP0) << 4 : 0u);
    u.tcgs[i][1] = static_cast<u32>(t.src);
    u.tcgs[i][2] = matrix;
    u.tcgs[i][3] = static_cast<u32>(t.normalize) | static_cast<u32>(post) << 1 | (static_cast<u32>(t.embossSrc) & 7) << 4;
  }

  // vertex format
  for (u32 a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; ++a) {
    const auto& m = config.attrs[a];
    if (m.attrType == GX_NONE) {
      continue;
    }
    u.attrs[a] = (static_cast<u32>(m.attrType) & 3) | (static_cast<u32>(m.cnt) & 15) << 2 |
                 (static_cast<u32>(m.compType) & 7) << 6 | static_cast<u32>(m.le) << 9 | static_cast<u32>(m.nbt3) << 10 |
                 (static_cast<u32>(m.frac) & 31) << 11 | static_cast<u32>(m.offset) << 16 |
                 static_cast<u32>(m.stride) << 24;
  }

  for (u32 i = 0; i < MaxIndStages; ++i) {
    const auto& s = config.indStages[i];
    u.indStages[i] = (static_cast<u32>(s.texCoordId) & 15) | (static_cast<u32>(s.texMapId) & 15) << 4 |
                     (static_cast<u32>(s.scaleS) & 15) << 8 | (static_cast<u32>(s.scaleT) & 15) << 12 |
                     static_cast<u32>(info.usedIndStages.test(i)) << 16;
  }

  u.counts[0] = stageCount;
  u.counts[1] = g_gxState.numTexGens;
  u.counts[2] = config.numIndStages;
  u.counts[3] = config.vtxStride;
  const auto& ac = config.alphaCompare;
  u.flags[0] = (static_cast<u32>(ac.comp0) & 7) | (static_cast<u32>(ac.op) & 3) << 3 |
               (static_cast<u32>(ac.comp1) & 7) << 5 | (ac.ref0 & 0xFF) << 8 | (ac.ref1 & 0xFF) << 16;
  u.flags[1] = (static_cast<u32>(config.fogType) & 15) | static_cast<u32>(config.fogRangeEnabled) << 4;
  for (u32 e = 0; e < MaxTevSwap; ++e) {
    const auto& sw = config.tevSwapTable[e];
    u.flags[2] |= ((static_cast<u32>(sw.red) & 3) | (static_cast<u32>(sw.green) & 3) << 2 |
                   (static_cast<u32>(sw.blue) & 3) << 4 | (static_cast<u32>(sw.alpha) & 3) << 6)
                  << (e * 8);
  }
  if (stageCount != 0) {
    const auto& last = config.tevStages[stageCount - 1];
    const u32 lastColor = static_cast<u32>(last.colorOp.outReg) & 3;
    const u32 lastAlpha = static_cast<u32>(last.alphaOp.outReg) & 3;
    u.flags[3] = static_cast<u32>(!colorNormalized[lastColor]) << 1 | static_cast<u32>(!alphaNormalized[lastAlpha]) << 2 |
                 lastColor << 3 | lastAlpha << 5;
  }
  u.flags[3] |= static_cast<u32>(info.usedIndStages.any());
  u.flags[3] |= static_cast<u32>(config.attrs[GX_VA_NRM].attrType != GX_NONE) << 7;
  u.nbtCache[0] = g_gxState.nbtCache[0];
  u.nbtCache[1] = g_gxState.nbtCache[1];
  return gfx::push_uniform(reinterpret_cast<const u8*>(&u), sizeof(u));
}
} // namespace aurora::gx
