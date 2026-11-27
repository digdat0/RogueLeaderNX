#include "uber.hpp"

#include "../gfx/hash.hpp"
#include "../webgpu/gpu.hpp"
#include "wgsl_helpers.hpp"

#include <string>
#include <string_view>

#include <tracy/Tracy.hpp>

namespace aurora::gx {
UberMode g_uberMode = UberMode::Hybrid;

namespace {
void replace_all(std::string& s, std::string_view token, std::string_view value) {
  for (size_t pos = s.find(token); pos != std::string::npos; pos = s.find(token, pos + value.size())) {
    s.replace(pos, token.size(), value);
  }
}

// Attribute loads for one storage buffer (direct data in vbuf, indexed arrays in abuf).
constexpr std::string_view FetchTemplate = R"(
fn fetch_num_BUF(off: u32, ct: u32, cnt: u32, frac: u32, le: bool) -> vec4f {
    switch (ct) {
        case 0u: {
            if (cnt == 1u) { return vec4f(fetch_u8_1(&BUF, off, frac, le), 0.0, 0.0, 0.0); }
            if (cnt == 2u) { return vec4f(fetch_u8_2(&BUF, off, frac, le), 0.0, 0.0); }
            if (cnt == 3u) { return vec4f(fetch_u8_3(&BUF, off, frac, le), 0.0); }
            return fetch_u8_4(&BUF, off, frac, le);
        }
        case 1u: {
            if (cnt == 1u) { return vec4f(fetch_s8_1(&BUF, off, frac, le), 0.0, 0.0, 0.0); }
            if (cnt == 2u) { return vec4f(fetch_s8_2(&BUF, off, frac, le), 0.0, 0.0); }
            if (cnt == 3u) { return vec4f(fetch_s8_3(&BUF, off, frac, le), 0.0); }
            return fetch_s8_4(&BUF, off, frac, le);
        }
        case 2u: {
            if (cnt == 1u) { return vec4f(fetch_u16_1(&BUF, off, frac, le), 0.0, 0.0, 0.0); }
            if (cnt == 2u) { return vec4f(fetch_u16_2(&BUF, off, frac, le), 0.0, 0.0); }
            if (cnt == 3u) { return vec4f(fetch_u16_3(&BUF, off, frac, le), 0.0); }
            return fetch_u16_4(&BUF, off, frac, le);
        }
        case 3u: {
            if (cnt == 1u) { return vec4f(fetch_s16_1(&BUF, off, frac, le), 0.0, 0.0, 0.0); }
            if (cnt == 2u) { return vec4f(fetch_s16_2(&BUF, off, frac, le), 0.0, 0.0); }
            if (cnt == 3u) { return vec4f(fetch_s16_3(&BUF, off, frac, le), 0.0); }
            return fetch_s16_4(&BUF, off, frac, le);
        }
        case 4u: {
            if (cnt == 1u) { return vec4f(fetch_f32_1(&BUF, off, le), 0.0, 0.0, 0.0); }
            if (cnt == 2u) { return vec4f(fetch_f32_2(&BUF, off, le), 0.0, 0.0); }
            if (cnt == 3u) { return vec4f(fetch_f32_3(&BUF, off, le), 0.0); }
            return fetch_f32_4(&BUF, off, le);
        }
        default: {
            return unpack4x8unorm(load_u32_raw(&BUF, off));
        }
    }
}

fn fetch_clr_BUF(off: u32, ct: u32, le: bool) -> vec4f {
    switch (ct) {
        case 0u: { return fetch_rgb565(&BUF, off, le); }
        case 1u: { return fetch_rgb8(&BUF, off, le); }
        case 2u: { return fetch_rgbx8(&BUF, off, le); }
        case 3u: { return fetch_rgba4(&BUF, off, le); }
        case 4u: { return fetch_rgba6(&BUF, off, le); }
        default: { return fetch_rgba8(&BUF, off, le); }
    }
}
)";

// The ubershader. Layout of the uniform block: see UberUniform in shader_info.cpp (they must
// agree byte for byte). Packed words (bit offsets):
//   attrs[a] (GXAttr order): type 0-1, count 2-5, component type 6-8, little endian 9, NBT3 10,
//     fraction 11-15, offset in the vertex 16-23, array stride 24-31
//   stages[i].x: color args a b c d (4 bits each) 0-15, alpha args a b c d (3 bits) 16, 19, 22, 25
//   stages[i].y: color op 0-3, bias 4-5, scale 6-7, clamp 8, out reg 9-10, wrap a b c 11-13;
//     alpha op from 16 the same, wrap a b c 27-29
//   stages[i].z: kcsel 0-4, kasel 5-9, tex map 10-13 (15 none), tex coord 14-17 (15 none), tex swap
//     18-19, ras swap 20-21, ras source 22-24 (0/1 channel, 2 zero, 3 bump, 4 bump normalized),
//     samples a texture 25
//   stages[i].w: indirect stage 0-1, format 2-3, bias sel 4-6, matrix kind 9-10 (0 none, 1 static,
//     2 dynamic S, 3 dynamic T), matrix 11-12, wrap S 13-15, wrap T 16-18, add prev 19, needs the
//     TEV texcoord 20, base coord 22, simple coords 23, base texture 24, alpha bump 25, its
//     component 26-27
//   tcgs[i]: x type (0 3x4, 1 2x4, 2 bump, 3 SRTG) | bump light << 4; y source (GXTexGenSrc);
//     z matrix mode (0 identity, 1 row, 2 per-vertex index) | row << 4; w normalize | post << 1 |
//     emboss source << 4
//   chans[c]: lighting 0, material from vertex 1, ambient from vertex 2, diffuse fn 3-4, attenuation
//     fn 5-6, light mask 8-15
//   ind_stages[i]: tex coord 0-3, tex map 4-7, scale S 8-11, scale T 12-15, used 16
//   counts: TEV stages, texgens, indirect stages, vertex stride
//   flags.x alpha compare: comp0 0-2, op 3-4, comp1 5-7, ref0 8-15, ref1 16-23; flags.y fog type
//     0-3, range 4; flags.z swap table (2 bits per channel, 8 per entry); flags.w: indirect stages
//     used 0, wrap result color 1, alpha 2, last color out 3-4, last alpha out 5-6, normals 7
constexpr std::string_view UberTemplate = R"(
struct Immediate {
    vtx_start: u32,
    current_pnmtx: u32,
    fog_range_base: u32,
    _pad: u32,
    array_start0: vec4u,
    array_start1: vec4u,
    array_start2: vec4u,
};
var<immediate> imm: Immediate;

struct Light {
    pos: vec3f,
    dir: vec3f,
    color: vec4f,
    cos_att: vec3f,
    dist_att: vec3f,
};

struct Fog {
    color: vec4f,
    a: f32,
    b: f32,
    c: f32,
    range_center: f32,
    range_k: array<vec4f, 3>,
};

struct Uniform {
    render_viewport_size: vec2f,
    logical_viewport_size: vec2f,
    line: vec4f,
    proj: mat4x4f,
    depth_range: vec4f,
    zfreeze_plane: vec4f,
    postex_mtx: array<mat3x4f, 21>,
    nrm_mtx: array<mat3x4f, 10>,
    tcg_post: array<mat3x4f, 8>,
    lights: array<Light, 8>,
    tev_regs: array<vec4f, 4>,
    kcolors: array<vec4f, 4>,
    chan_amb: array<vec4f, 4>,
    chan_mat: array<vec4f, 4>,
    fog: Fog,
    texcoord_scale: array<vec4f, 8>,
    ind_mtx: array<mat2x4f, 3>,
    tex_size_bias: array<vec4f, 8>,
    stages: array<vec4u, 16>,
    tcgs: array<vec4u, 8>,
    attrs: array<vec4u, 6>,
    chans: vec4u,
    ind_stages: vec4u,
    counts: vec4u,
    flags: vec4u,
    nbt_cache: array<vec4f, 2>,
};

@group(0) @binding(0)
var<storage, read> vbuf: array<u32>;
@group(0) @binding(1)
var<storage, read> abuf: array<u32>;
@group(1) @binding(0)
var<uniform> ubuf: Uniform;
@group(2) @binding(0) var tex0: texture_2d<f32>;
@group(2) @binding(1) var tex0_samp: sampler;
@group(2) @binding(2) var tex1: texture_2d<f32>;
@group(2) @binding(3) var tex1_samp: sampler;
@group(2) @binding(4) var tex2: texture_2d<f32>;
@group(2) @binding(5) var tex2_samp: sampler;
@group(2) @binding(6) var tex3: texture_2d<f32>;
@group(2) @binding(7) var tex3_samp: sampler;
@group(2) @binding(8) var tex4: texture_2d<f32>;
@group(2) @binding(9) var tex4_samp: sampler;
@group(2) @binding(10) var tex5: texture_2d<f32>;
@group(2) @binding(11) var tex5_samp: sampler;
@group(2) @binding(12) var tex6: texture_2d<f32>;
@group(2) @binding(13) var tex6_samp: sampler;
@group(2) @binding(14) var tex7: texture_2d<f32>;
@group(2) @binding(15) var tex7_samp: sampler;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) cc0: vec4f,
    @location(1) cc1: vec4f,
    @location(2) tex0: vec3f,
    @location(3) tex1: vec3f,
    @location(4) tex2: vec3f,
    @location(5) tex3: vec3f,
    @location(6) tex4: vec3f,
    @location(7) tex5: vec3f,
    @location(8) tex6: vec3f,
    @location(9) tex7: vec3f,%NRM_OUT_DECL%
};
%FRAG_OUTPUT_STRUCT%
fn attr_word(a: u32) -> u32 {
    return ubuf.attrs[a / 4u][a % 4u];
}

// The immediates are read whole at constant offsets and the component picked in registers:
// indexing a vec4 of the immediate block with a run-time index makes a push constant load with
// a dynamic offset, and the Mesa 26.2 development RADV that stable SteamOS 3.8.28 ships
// (vulkan-radeon 26.2.0_devel) took a garbage push constant size from it: the first draw with an
// uber pipeline copied gigabytes in radv_upload_push_constants and crashed (Mesa 25.2.8: fine).
fn array_start(a: u32) -> u32 {
    let i = a - 9u;
    let v = select(select(imm.array_start2, imm.array_start1, i < 8u), imm.array_start0, i < 4u);
    let c = i & 3u;
    return select(select(select(v.w, v.z, c == 2u), v.y, c == 1u), v.x, c == 0u);
}

// Byte offset of attribute a of vertex v: in abuf when indexed, in vbuf when direct.
fn attr_offset(a: u32, w: u32, v: u32, dl_extra: u32, within: u32) -> u32 {
    let t = w & 3u;
    let dl = imm.vtx_start + v * ubuf.counts.w + extractBits(w, 16u, 8u) + dl_extra;
    if (t == 2u) {
        return array_start(a) + raw_fetch_u8_1(&vbuf, dl) * extractBits(w, 24u, 8u) + within;
    }
    if (t == 3u) {
        return array_start(a) + raw_fetch_u16_1(&vbuf, dl, false) * extractBits(w, 24u, 8u) + within;
    }
    return dl + within;
}

fn fetch_num(w: u32, off: u32, cnt: u32) -> vec4f {
    let ct = extractBits(w, 6u, 3u);
    let frac = extractBits(w, 11u, 5u);
    let le = extractBits(w, 9u, 1u) != 0u;
    if ((w & 3u) >= 2u) {
        return fetch_num_abuf(off, ct, cnt, frac, le);
    }
    return fetch_num_vbuf(off, ct, cnt, frac, le);
}

fn load_pos(v: u32) -> vec3f {
    let w = attr_word(9u);
    if ((w & 3u) == 0u) { return vec3f(0.0); }
    let cnt = extractBits(w, 2u, 4u);
    let r = fetch_num(w, attr_offset(9u, w, v, 0u, 0u), cnt);
    if (cnt == 2u) { return vec3f(r.xy, 0.0); }
    return r.xyz;
}

fn load_nrm(v: u32) -> vec3f {
    let w = attr_word(10u);
    if ((w & 3u) == 0u) { return vec3f(1.0, 0.0, 0.0); }
    return fetch_num(w, attr_offset(10u, w, v, 0u, 0u), min(extractBits(w, 2u, 4u), 3u)).xyz;
}

// Binormal (slice 1) / tangent (slice 2) of NBT normals; without them the ones the vertex unit
// last held (GXState::nbtCache).
fn load_nbt(v: u32, slice: u32) -> vec3f {
    let w = attr_word(10u);
    let t = w & 3u;
    if (t == 0u || extractBits(w, 2u, 4u) != 9u) { return ubuf.nbt_cache[slice - 1u].xyz; }
    let ct = extractBits(w, 6u, 3u);
    let compsize = select(select(4u, 2u, ct == 2u || ct == 3u), 1u, ct <= 1u);
    var dl_extra = 0u;
    if (extractBits(w, 10u, 1u) != 0u) {
        if (t == 2u) {
            dl_extra = slice;
        } else if (t == 3u) {
            dl_extra = slice * 2u;
        }
    }
    return fetch_num(w, attr_offset(10u, w, v, dl_extra, slice * 3u * compsize), 3u).xyz;
}

fn load_clr(v: u32, i: u32) -> vec4f {
    let a = 11u + i;
    let w = attr_word(a);
    if ((w & 3u) == 0u) { return vec4f(0.0); }
    let off = attr_offset(a, w, v, 0u, 0u);
    let ct = extractBits(w, 6u, 3u);
    let le = extractBits(w, 9u, 1u) != 0u;
    if ((w & 3u) >= 2u) {
        return fetch_clr_abuf(off, ct, le);
    }
    return fetch_clr_vbuf(off, ct, le);
}

fn load_tex(v: u32, i: u32) -> vec2f {
    let a = 13u + i;
    let w = attr_word(a);
    if ((w & 3u) == 0u) { return vec2f(0.0); }
    let cnt = extractBits(w, 2u, 4u);
    let r = fetch_num(w, attr_offset(a, w, v, 0u, 0u), cnt);
    if (cnt == 1u) { return vec2f(r.x, 0.0); }
    return r.xy;
}

fn load_pnmtxidx(v: u32) -> u32 {
    let w = attr_word(0u);
    if ((w & 3u) == 0u) { return imm.current_pnmtx; }
    return raw_fetch_u8_1(&vbuf, attr_offset(0u, w, v, 0u, 0u)) / 3u;
}

fn load_texmtxidx(v: u32, i: u32) -> u32 {
    let w = attr_word(1u + i);
    return raw_fetch_u8_1(&vbuf, attr_offset(1u + i, w, v, 0u, 0u));
}

// Color channel c (COLOR0, COLOR1, ALPHA0, ALPHA1): material, or lit by its lights.
fn channel(c: u32, vtx: vec4f, mv_pos: vec3f, mv_nrm: vec3f) -> vec4f {
    let cfg = ubuf.chans[c];
    let mat = select(ubuf.chan_mat[c], vtx, (cfg & 2u) != 0u);
    if ((cfg & 1u) == 0u) { return mat; }
    let amb = select(ubuf.chan_amb[c], vtx, (cfg & 4u) != 0u);
    let diff_fn = extractBits(cfg, 3u, 2u);
    let attn_fn = extractBits(cfg, 5u, 2u);
    var lighting = amb;
    for (var i = 0u; i < 8u; i++) {
        if ((cfg & (1u << (8u + i))) == 0u) { continue; }
        let light = ubuf.lights[i];
        var ldir = light.pos - mv_pos;
        let dist2 = dot(ldir, ldir);
        let dist = sqrt(dist2);
        ldir = ldir / dist;
        var attn = 1.0;
        if (attn_fn == 1u) {
            let cosine = max(0.0, dot(ldir, light.dir));
            let cos_attn = dot(light.cos_att, vec3f(1.0, cosine, cosine * cosine));
            let dist_attn = dot(light.dist_att, vec3f(1.0, dist, dist2));
            attn = max(0.0, cos_attn / dist_attn);
        } else if (attn_fn == 0u) {
            attn = select(0.0, max(0.0, dot(mv_nrm, light.dir)), dot(mv_nrm, ldir) >= 0.0);
            let cos_attn = dot(light.cos_att, vec3f(1.0, attn, attn * attn));
            var dist_attn: f32;
            if (diff_fn != 0u) {
                dist_attn = max(0.0, dot(normalize(light.dist_att), vec3f(1.0, attn, attn * attn)));
            } else {
                dist_attn = max(0.0, dot(light.dist_att, vec3f(1.0, attn, attn * attn)));
            }
            attn = max(0.0, cos_attn / dist_attn);
        }
        var diff = 1.0;
        if (diff_fn == 1u) {
            diff = dot(ldir, mv_nrm);
        } else if (diff_fn == 2u) {
            diff = max(0.0, dot(ldir, mv_nrm));
        }
        lighting = lighting + (attn * diff * light.color);
    }
    return mat * clamp(lighting, vec4f(0.0), vec4f(1.0));
}

@vertex
fn vs_main(
    @builtin(vertex_index) vidx: u32%VTX_IN%
) -> VertexOutput {
    var out: VertexOutput;%VTX_PRE%
    if (out.pos.z >= 0.0 && out.pos.z <= out.pos.w) {
        out.pos.z = clamp(out.pos.w * ubuf.depth_range.x + out.pos.z * ubuf.depth_range.y, 0.0, out.pos.w);
    }
    let in_nrm = load_nrm(in_vidx);
    let nrm_tmp = vec4f(in_nrm, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];
    let mv_nrm = select(nrm_tmp, normalize(nrm_tmp), dot(nrm_tmp, nrm_tmp) > 1e-10);%NRM_OUT%
    let in_clr0 = load_clr(in_vidx, 0u);
    let in_clr1 = load_clr(in_vidx, 1u);
    var cc0 = channel(0u, in_clr0, mv_pos, mv_nrm);
    cc0.a = channel(2u, in_clr0, mv_pos, mv_nrm).a;
    var cc1 = channel(1u, in_clr1, mv_pos, mv_nrm);
    cc1.a = channel(3u, in_clr1, mv_pos, mv_nrm).a;
    out.cc0 = cc0;
    out.cc1 = cc1;
    let in_binrm = load_nbt(in_vidx, 1u);
    let in_tangent = load_nbt(in_vidx, 2u);
    let line_mask = bitcast<u32>(ubuf.line.w);
    var tcsrc: array<vec4f, 8>;
    var tcproj: array<vec3f, 8>;
    var tco: array<vec3f, 8>;
    for (var i = 0u; i < 8u; i++) {
        tco[i] = vec3f(0.0, 0.0, 1.0);
    }
    let num_texgens = min(ubuf.counts.y, 8u);
    for (var i = 0u; i < num_texgens; i++) {
        let g = ubuf.tcgs[i];
        let kind = g.x & 15u;
        if (kind == 2u) { continue; }
        let src = g.y;
        var tc = vec4f(0.0);
        if (src >= 4u && src <= 11u) {
            tc = vec4f(load_tex(in_vidx, src - 4u), 1.0, 1.0);
        } else if (src >= 12u && src <= 18u) {
            tc = tcsrc[src - 12u];
        } else if (src == 0u) {
            tc = vec4f(in_pos, 1.0);
        } else if (src == 1u) {
            tc = vec4f(in_nrm, 1.0);
        } else if (src == 2u) {
            tc = vec4f(in_binrm, 1.0);
        } else if (src == 3u) {
            tc = vec4f(in_tangent, 1.0);
        } else if (src == 19u) {
            tc = cc0;
        } else if (src == 20u) {
            tc = cc1;
        }
        tcsrc[i] = tc;
        var t: vec3f;
        if (kind <= 1u) {
            let mode = g.z & 3u;
            if (mode == 2u) {
                t = tc * ubuf.postex_mtx[load_texmtxidx(in_vidx, i) / 3u];
            } else if (mode == 1u) {
                t = tc * ubuf.postex_mtx[g.z >> 4u];
            } else {
                t = tc.xyz;
            }
            if (kind == 1u) {
                t.z = 1.0;
            }
        } else {
            t = vec3f(tc.xy, 1.0);
        }
        if ((g.w & 1u) != 0u) {
            t = normalize(t);
        }
        var p = t;
        if ((g.w & 2u) != 0u) {
            p = vec4f(t, 1.0) * ubuf.tcg_post[i];
        }%TC_OFFSET%
        tcproj[i] = p;
        if (kind == 0u) {
            tco[i] = p;
        } else {
            tco[i] = vec3f(p.xy, 1.0);
        }
    }
    // Emboss: the source texcoord shifted by the light projected onto tangent and binormal.
    for (var i = 0u; i < num_texgens; i++) {
        let g = ubuf.tcgs[i];
        if ((g.x & 15u) != 2u) { continue; }
        let ldir = normalize(ubuf.lights[(g.x >> 4u) & 7u].pos - mv_pos);
        let btan = vec4f(in_tangent, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];
        let bbin = vec4f(in_binrm, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];
        tco[i] = vec3f(tcproj[(g.w >> 4u) & 7u].xy + vec2f(dot(ldir, btan), dot(ldir, bbin)), 1.0);
    }
    out.tex0 = tco[0];
    out.tex1 = tco[1];
    out.tex2 = tco[2];
    out.tex3 = tco[3];
    out.tex4 = tco[4];
    out.tex5 = tco[5];
    out.tex6 = tco[6];
    out.tex7 = tco[7];
    return out;
}

var<private> regs: array<vec4f, 4>;

fn sample_map(m: u32, uv: vec2f) -> vec4f {
    switch (m) {
        case 0u: { return textureSampleBias(tex0, tex0_samp, uv, ubuf.tex_size_bias[0].z); }
        case 1u: { return textureSampleBias(tex1, tex1_samp, uv, ubuf.tex_size_bias[1].z); }
        case 2u: { return textureSampleBias(tex2, tex2_samp, uv, ubuf.tex_size_bias[2].z); }
        case 3u: { return textureSampleBias(tex3, tex3_samp, uv, ubuf.tex_size_bias[3].z); }
        case 4u: { return textureSampleBias(tex4, tex4_samp, uv, ubuf.tex_size_bias[4].z); }
        case 5u: { return textureSampleBias(tex5, tex5_samp, uv, ubuf.tex_size_bias[5].z); }
        case 6u: { return textureSampleBias(tex6, tex6_samp, uv, ubuf.tex_size_bias[6].z); }
        case 7u: { return textureSampleBias(tex7, tex7_samp, uv, ubuf.tex_size_bias[7].z); }
        default: { return vec4f(1.0); }
    }
}

fn swap4(v: vec4f, entry: u32) -> vec4f {
    let t = extractBits(ubuf.flags.z, entry * 8u, 8u);
    return vec4f(v[t & 3u], v[(t >> 2u) & 3u], v[(t >> 4u) & 3u], v[(t >> 6u) & 3u]);
}

fn ras_value(code: u32, entry: u32, rast0: vec4f, rast1: vec4f, bump: f32) -> vec4f {
    switch (code) {
        case 0u: { return swap4(rast0, entry); }
        case 1u: { return swap4(rast1, entry); }
        case 3u: { return vec4f(bump); }
        case 4u: { return vec4f(bump * (255.0 / 248.0)); }
        default: { return vec4f(0.0); }
    }
}

fn konst_c(sel: u32) -> vec3f {
    if (sel < 8u) { return vec3f(f32(8u - sel) / 8.0); }
    let k = ubuf.kcolors[sel & 3u];
    switch (sel >> 2u) {
        case 3u: { return k.rgb; }
        case 4u: { return vec3f(k.r); }
        case 5u: { return vec3f(k.g); }
        case 6u: { return vec3f(k.b); }
        default: { return vec3f(k.a); }
    }
}

fn konst_a(sel: u32) -> f32 {
    if (sel < 8u) { return f32(8u - sel) / 8.0; }
    let k = ubuf.kcolors[sel & 3u];
    switch (sel >> 2u) {
        case 4u: { return k.r; }
        case 5u: { return k.g; }
        case 6u: { return k.b; }
        default: { return k.a; }
    }
}

fn color_arg(sel: u32, tex: vec4f, ras: vec4f, konst: vec3f) -> vec3f {
    switch (sel) {
        case 0u: { return regs[0].rgb; }
        case 1u: { return vec3f(regs[0].a); }
        case 2u: { return regs[1].rgb; }
        case 3u: { return vec3f(regs[1].a); }
        case 4u: { return regs[2].rgb; }
        case 5u: { return vec3f(regs[2].a); }
        case 6u: { return regs[3].rgb; }
        case 7u: { return vec3f(regs[3].a); }
        case 8u: { return tex.rgb; }
        case 9u: { return vec3f(tex.a); }
        case 10u: { return ras.rgb; }
        case 11u: { return vec3f(ras.a); }
        case 12u: { return vec3f(1.0); }
        case 13u: { return vec3f(0.5); }
        case 14u: { return konst; }
        default: { return vec3f(0.0); }
    }
}

fn alpha_arg(sel: u32, tex: vec4f, ras: vec4f, konst: f32) -> f32 {
    switch (sel) {
        case 0u: { return regs[0].a; }
        case 1u: { return regs[1].a; }
        case 2u: { return regs[2].a; }
        case 3u: { return regs[3].a; }
        case 4u: { return tex.a; }
        case 5u: { return ras.a; }
        case 6u: { return konst; }
        default: { return 0.0; }
    }
}

fn tev_bias_value(code: u32) -> f32 {
    if (code == 1u) { return 0.5; }
    if (code == 2u) { return -0.5; }
    return 0.0;
}

fn tev_scale_value(code: u32) -> f32 {
    switch (code) {
        case 1u: { return 2.0; }
        case 2u: { return 4.0; }
        case 3u: { return 0.5; }
        default: { return 1.0; }
    }
}

fn tev_color_op(cfg: u32, a: vec3f, b: vec3f, c: vec3f, d: vec3f) -> vec3f {
    let op = cfg & 15u;
    var r: vec3f;
    switch (op) {
        case 0u, 1u: {
            let m = mix(a, b, c);
            r = ((select(m, -m, op == 1u) + d) + tev_bias_value(extractBits(cfg, 4u, 2u))) *
                tev_scale_value(extractBits(cfg, 6u, 2u));
        }
        case 8u: { r = select(vec3f(0.0), c, round(a.r * 255.0) > round(b.r * 255.0)) + d; }
        case 9u: { r = select(vec3f(0.0), c, round(a.r * 255.0) == round(b.r * 255.0)) + d; }
        case 10u: {
            r = select(vec3f(0.0), c, round(dot(a.rg * 255.0, vec2(1.0, 256.0))) > round(dot(b.rg * 255.0, vec2(1.0, 256.0)))) + d;
        }
        case 11u: {
            r = select(vec3f(0.0), c, round(dot(a.rg * 255.0, vec2(1.0, 256.0))) == round(dot(b.rg * 255.0, vec2(1.0, 256.0)))) + d;
        }
        case 12u: {
            r = select(vec3f(0.0), c, round(dot(a.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) > round(dot(b.rgb * 255.0, vec3(1.0, 256.0, 65536.0)))) + d;
        }
        case 13u: {
            r = select(vec3f(0.0), c, round(dot(a.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) == round(dot(b.rgb * 255.0, vec3(1.0, 256.0, 65536.0)))) + d;
        }
        case 14u: { r = select(vec3f(0.0), c, round(a * 255.0) > round(b * 255.0)) + d; }
        default: { r = select(vec3f(0.0), c, round(a * 255.0) == round(b * 255.0)) + d; }
    }
    if (extractBits(cfg, 8u, 1u) != 0u) {
        return clamp(r, vec3f(0.0), vec3f(1.0));
    }
    return clamp(r, vec3f(-4.0), vec3f(4.0));
}

// ca/cb: the stage's color A/B (before its color result), compared by the packed alpha ops.
fn tev_alpha_op(cfg: u32, a: f32, b: f32, c: f32, d: f32, ca: vec3f, cb: vec3f) -> f32 {
    let op = cfg & 15u;
    var r: f32;
    switch (op) {
        case 0u, 1u: {
            let m = mix(a, b, c);
            r = ((select(m, -m, op == 1u) + d) + tev_bias_value(extractBits(cfg, 4u, 2u))) *
                tev_scale_value(extractBits(cfg, 6u, 2u));
        }
        case 8u: { r = select(0.0, c, round(ca.r * 255.0) > round(cb.r * 255.0)) + d; }
        case 9u: { r = select(0.0, c, round(ca.r * 255.0) == round(cb.r * 255.0)) + d; }
        case 10u: {
            r = select(0.0, c, round(dot(ca.rg * 255.0, vec2(1.0, 256.0))) > round(dot(cb.rg * 255.0, vec2(1.0, 256.0)))) + d;
        }
        case 11u: {
            r = select(0.0, c, round(dot(ca.rg * 255.0, vec2(1.0, 256.0))) == round(dot(cb.rg * 255.0, vec2(1.0, 256.0)))) + d;
        }
        case 12u: {
            r = select(0.0, c, round(dot(ca.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) > round(dot(cb.rgb * 255.0, vec3(1.0, 256.0, 65536.0)))) + d;
        }
        case 13u: {
            r = select(0.0, c, round(dot(ca.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) == round(dot(cb.rgb * 255.0, vec3(1.0, 256.0, 65536.0)))) + d;
        }
        case 14u: { r = select(0.0, c, round(a * 255.0) > round(b * 255.0)) + d; }
        default: { r = select(0.0, c, round(a * 255.0) == round(b * 255.0)) + d; }
    }
    if (extractBits(cfg, 8u, 1u) != 0u) {
        return clamp(r, 0.0, 1.0);
    }
    return clamp(r, -4.0, 4.0);
}

fn ind_wrap(code: u32, v: f32) -> f32 {
    switch (code) {
        case 1u: { return v % 256.0; }
        case 2u: { return v % 128.0; }
        case 3u: { return v % 64.0; }
        case 4u: { return v % 32.0; }
        case 5u: { return v % 16.0; }
        case 6u: { return 0.0; }
        default: { return v; }
    }
}

fn alpha_test(comp: u32, v: u32, ref_value: u32) -> bool {
    switch (comp) {
        case 0u: { return false; }
        case 1u: { return v < ref_value; }
        case 2u: { return v == ref_value; }
        case 3u: { return v <= ref_value; }
        case 4u: { return v > ref_value; }
        case 5u: { return v != ref_value; }
        case 6u: { return v >= ref_value; }
        default: { return true; }
    }
}

@fragment
fn fs_main(in: VertexOutput) -> %FRAG_OUT_TYPE% {%FRAG_PRE%
    regs = ubuf.tev_regs;
    var tuv: array<vec2f, 8>;
    tuv[0] = in.tex0.xy / in.tex0.z;
    tuv[1] = in.tex1.xy / in.tex1.z;
    tuv[2] = in.tex2.xy / in.tex2.z;
    tuv[3] = in.tex3.xy / in.tex3.z;
    tuv[4] = in.tex4.xy / in.tex4.z;
    tuv[5] = in.tex5.xy / in.tex5.z;
    tuv[6] = in.tex6.xy / in.tex6.z;
    tuv[7] = in.tex7.xy / in.tex7.z;
    let fl = ubuf.flags.w;
    let any_ind = (fl & 1u) != 0u;
    var ind: array<vec3f, 4>;
    let num_ind = min(ubuf.counts.z, 4u);
    for (var i = 0u; i < num_ind; i++) {
        let s = ubuf.ind_stages[i];
        if ((s & 0x10000u) == 0u) { continue; }
        let tc = s & 7u;
        let tm = extractBits(s, 4u, 4u);
        let scale = vec2f(1.0 / f32(1u << extractBits(s, 8u, 4u)), 1.0 / f32(1u << extractBits(s, 12u, 4u)));
        let uv = tuv[tc] * ubuf.texcoord_scale[tc].xy * scale / ubuf.tex_size_bias[tm & 7u].xy;
        ind[i] = 255.0 * sample_map(tm, uv).abg;
    }
    var bump_masks = array<u32, 4>(0xF8u, 0xE0u, 0xF0u, 0xF8u);
    var t_texcoord = vec2f(0.0);
    let num_stages = min(ubuf.counts.x, 16u);
    for (var i = 0u; i < num_stages; i++) {
        let st = ubuf.stages[i];
        let sel = st.z;
        let indc = st.w;
        let tm = extractBits(sel, 10u, 4u);
        let tc = extractBits(sel, 14u, 4u) & 7u;
        let sample_flag = extractBits(sel, 25u, 1u) != 0u;
        var uv_in = vec2f(0.0);
        var have_uv = false;
        if (extractBits(indc, 20u, 1u) != 0u) {
            let kind = extractBits(indc, 9u, 2u);
            var offset = vec2f(0.0);
            if (kind != 0u) {
                let fmt = extractBits(indc, 2u, 2u);
                var ic = ind[extractBits(indc, 0u, 2u)];
                if (fmt != 0u) {
                    ic = floor(ic / f32(4u << fmt));
                }
                let bias_sel = extractBits(indc, 4u, 3u);
                if (bias_sel != 0u) {
                    let bias = select(1.0, -128.0, fmt == 0u);
                    ic = ic + vec3f(f32(bias_sel & 1u), f32((bias_sel >> 1u) & 1u), f32((bias_sel >> 2u) & 1u)) * bias;
                }
                let m = extractBits(indc, 11u, 2u);
                if (kind == 1u) {
                    let c0 = ubuf.ind_mtx[m][0];
                    let c1 = ubuf.ind_mtx[m][1];
                    offset = vec2f(dot(vec3f(c0.xz, c1.x), ic), dot(vec3f(c0.yw, c1.y), ic)) * c1.z;
                } else if (kind == 2u) {
                    offset = tuv[tc] * ubuf.texcoord_scale[tc].xy * ic.x * ubuf.ind_mtx[m][1][2] / 256.0;
                } else {
                    offset = tuv[tc] * ubuf.texcoord_scale[tc].xy * ic.y * ubuf.ind_mtx[m][1][2] / 256.0;
                }
            }
            let simple = extractBits(indc, 23u, 1u) != 0u;
            let has_base = extractBits(indc, 22u, 1u) != 0u;
            var final_coord = offset;
            if (has_base) {
                var base = tuv[tc];
                if (!simple) {
                    base = base * ubuf.texcoord_scale[tc].xy;
                }
                let ws = extractBits(indc, 13u, 3u);
                let wt = extractBits(indc, 16u, 3u);
                if (ws != 0u || wt != 0u) {
                    base = vec2f(ind_wrap(ws, base.x), ind_wrap(wt, base.y));
                }
                final_coord = select(base, base + offset, kind != 0u);
            }
            if (any_ind && (has_base || kind != 0u)) {
                if (extractBits(indc, 19u, 1u) != 0u) {
                    t_texcoord += final_coord;
                } else {
                    t_texcoord = final_coord;
                }
                if (sample_flag && extractBits(indc, 24u, 1u) != 0u) {
                    uv_in = select(t_texcoord / ubuf.tex_size_bias[tm & 7u].xy, t_texcoord, simple);
                    have_uv = true;
                }
            }
        }
        var tex = vec4f(1.0);
        if (sample_flag) {
            if (!have_uv) {
                uv_in = tuv[tc];
            }
            tex = swap4(sample_map(tm, uv_in), extractBits(sel, 18u, 2u));
        }
        var bump = 0.0;
        if (extractBits(indc, 25u, 1u) != 0u) {
            let v = ind[extractBits(indc, 0u, 2u)][extractBits(indc, 26u, 2u)];
            bump = f32(u32(v) & bump_masks[extractBits(indc, 2u, 2u)]) / 255.0;
        }
        let ras = ras_value(extractBits(sel, 22u, 3u), extractBits(sel, 20u, 2u), in.cc0, in.cc1, bump);
        let kc = konst_c(extractBits(sel, 0u, 5u));
        let ka = konst_a(extractBits(sel, 5u, 5u));
        let cin = st.x;
        let ops = st.y;
        var ca = color_arg(extractBits(cin, 0u, 4u), tex, ras, kc);
        var cb = color_arg(extractBits(cin, 4u, 4u), tex, ras, kc);
        var cc = color_arg(extractBits(cin, 8u, 4u), tex, ras, kc);
        let cd = color_arg(extractBits(cin, 12u, 4u), tex, ras, kc);
        if (extractBits(ops, 11u, 1u) != 0u) { ca = tev_overflow_vec3f(ca); }
        if (extractBits(ops, 12u, 1u) != 0u) { cb = tev_overflow_vec3f(cb); }
        if (extractBits(ops, 13u, 1u) != 0u) { cc = tev_overflow_vec3f(cc); }
        let color_out = extractBits(ops, 9u, 2u);
        regs[color_out] = vec4f(tev_color_op(ops & 0xFFFFu, ca, cb, cc, cd), regs[color_out].a);
        var aa = alpha_arg(extractBits(cin, 16u, 3u), tex, ras, ka);
        var ab = alpha_arg(extractBits(cin, 19u, 3u), tex, ras, ka);
        var ac = alpha_arg(extractBits(cin, 22u, 3u), tex, ras, ka);
        let ad = alpha_arg(extractBits(cin, 25u, 3u), tex, ras, ka);
        if (extractBits(ops, 27u, 1u) != 0u) { aa = tev_overflow_f32(aa); }
        if (extractBits(ops, 28u, 1u) != 0u) { ab = tev_overflow_f32(ab); }
        if (extractBits(ops, 29u, 1u) != 0u) { ac = tev_overflow_f32(ac); }
        regs[extractBits(ops, 25u, 2u)].a = tev_alpha_op(ops >> 16u, aa, ab, ac, ad, ca, cb);
    }
    let last_color = extractBits(fl, 3u, 2u);
    let last_alpha = extractBits(fl, 5u, 2u);
    if (last_color != 0u) {
        regs[0] = vec4f(regs[last_color].rgb, regs[0].a);
    }
    if (last_alpha != 0u) {
        regs[0].a = regs[last_alpha].a;
    }
    var prev = regs[0];
    let fog_type = extractBits(ubuf.flags.y, 0u, 4u);
    if (fog_type != 0u) {
        let fog_depth = %FOG_DEPTH%;
        var fog_base: f32;
        if ((fog_type & 8u) != 0u) {
            fog_base = ubuf.fog.a * fog_depth;
        } else {
            fog_base = ubuf.fog.a / (ubuf.fog.b - fog_depth);
        }
        if (extractBits(ubuf.flags.y, 4u, 1u) != 0u) {
            fog_base *= bitcast<f32>(abuf[imm.fog_range_base + u32(in.pos.x)]);
        }
        var fog_f = clamp(fog_base - ubuf.fog.c, 0.0, 1.0);
        var fog_z = fog_f;
        switch (fog_type & 7u) {
            case 4u: { fog_z = 1.0 - exp2(-8.0 * fog_f); }
            case 5u: { fog_z = 1.0 - exp2(-8.0 * fog_f * fog_f); }
            case 6u: { fog_z = exp2(-8.0 * (1.0 - fog_f)); }
            case 7u: {
                fog_f = 1.0 - fog_f;
                fog_z = exp2(-8.0 * fog_f * fog_f);
            }
            default: {}
        }
        prev = vec4f(mix(prev.rgb, ubuf.fog.color.rgb, clamp(fog_z, 0.0, 1.0)), prev.a);
    }
    let wrap_color = extractBits(fl, 1u, 1u) != 0u;
    let wrap_alpha = extractBits(fl, 2u, 1u) != 0u;
    if (wrap_color && wrap_alpha) {
        prev = tev_overflow_vec4f(prev);
    } else if (wrap_color) {
        prev = vec4f(tev_overflow_vec3f(prev.rgb), prev.a);
    } else if (wrap_alpha) {
        prev.a = tev_overflow_f32(prev.a);
    }
    let ac_cfg = ubuf.flags.x;
    let comp0 = extractBits(ac_cfg, 0u, 3u);
    let comp1 = extractBits(ac_cfg, 5u, 3u);
    if (comp0 != 7u || comp1 != 7u) {
        let alpha_value = u32(round(clamp(prev.a, 0.0, 1.0) * 255.0));
        let p0 = alpha_test(comp0, alpha_value, extractBits(ac_cfg, 8u, 8u));
        let p1 = alpha_test(comp1, alpha_value, extractBits(ac_cfg, 16u, 8u));
        var passed: bool;
        switch (extractBits(ac_cfg, 3u, 2u)) {
            case 0u: { passed = p0 && p1; }
            case 1u: { passed = p0 || p1; }
            case 2u: { passed = p0 != p1; }
            default: { passed = p0 == p1; }
        }
        if (!passed) {
            discard;
        }
    }%DST_ALPHA%%FRAG_RETURN%
}
)";
} // namespace

PipelineConfig uber_pipeline_config(const PipelineConfig& config) noexcept {
  PipelineConfig uber = config;
  uber.msaaSamples = UberMarker;
  uber.shaderConfig = {};
  uber.shaderConfig.lineMode = config.shaderConfig.lineMode;
  uber.shaderConfig.zFreeze = config.shaderConfig.zFreeze;
  // The destination alpha value reaches the blend constant at draw time; only whether there is one
  // (and whether it is 0, see create_pipeline) changes the pipeline.
  if (uber.dstAlpha != UINT32_MAX && uber.dstAlpha != 0) {
    uber.dstAlpha = 1;
  }
  return uber;
}

static std::string build_uber_shader_source(u8 lineMode, bool zFreeze, DstAlphaMode dstAlphaMode,
                                            uint32_t normalAttachment) {
  std::string src;
  if (dstAlphaMode == DstAlphaMode::DualSource) {
    src += "enable dual_source_blending;\n";
  }
  src += WgslHelpers;
  for (const std::string_view buf : {"vbuf", "abuf"}) {
    std::string fetch{FetchTemplate};
    replace_all(fetch, "BUF", buf);
    src += fetch;
  }
  std::string body{UberTemplate};

  // Vertex position, expanded to quads for lines and points as the specialized shaders do.
  std::string vtxIn;
  std::string vtxPre;
  std::string tcOffset;
  if (lineMode == 0) {
    vtxPre =
        "\n    let in_vidx = vidx;"
        "\n    let in_pnmtxidx = load_pnmtxidx(in_vidx);"
        "\n    let in_pos = load_pos(in_vidx);"
        "\n    let mv_pos = vec4f(in_pos, 1.0) * ubuf.postex_mtx[in_pnmtxidx];"
        "\n    out.pos = vec4f(mv_pos, 1.0) * ubuf.proj;";
  } else if (lineMode == 3) {
    vtxIn = ",\n    @builtin(instance_index) iidx: u32";
    vtxPre =
        "\n    let in_vidx = iidx;"
        "\n    let in_pos = load_pos(in_vidx);"
        "\n    let in_pnmtxidx = load_pnmtxidx(in_vidx);"
        "\n    let mv_pos = vec4f(in_pos, 1.0) * ubuf.postex_mtx[in_pnmtxidx];"
        "\n    let clip = vec4f(mv_pos, 1.0) * ubuf.proj;"
        "\n    let viewport_scale = ubuf.render_viewport_size / max(ubuf.logical_viewport_size, vec2f(1.0));"
        "\n    let point_size = ubuf.line.x * min(viewport_scale.x, viewport_scale.y);"
        "\n    let x_sign = select(-1.0, 1.0, (vidx & 1u) != 0u);"
        "\n    let y_sign = select(-1.0, 1.0, vidx >= 2u);"
        "\n    let offset_px = vec2f(x_sign, y_sign) * (point_size / 2.0);"
        "\n    let offset_ndc = (offset_px * 2.0) / ubuf.render_viewport_size;"
        "\n    out.pos = vec4f(clip.xy + offset_ndc * clip.w, clip.zw);";
    tcOffset =
        "\n        if ((line_mask & (1u << i)) != 0u) {"
        "\n            if ((vidx & 1u) != 0u) { p.x += ubuf.line.z; }"
        "\n            if (vidx >= 2u) { p.y += ubuf.line.z; }"
        "\n        }";
  } else {
    vtxIn = ",\n    @builtin(instance_index) iidx: u32";
    vtxPre = fmt::format(
        "\n    let use_b = vidx >= 2u;"
        "\n    let vidx_a = iidx * {}u;"
        "\n    let vidx_b = vidx_a + 1u;"
        "\n    let in_vidx = select(vidx_a, vidx_b, use_b);"
        "\n    let pos_a = load_pos(vidx_a);"
        "\n    let pos_b = load_pos(vidx_b);"
        "\n    let in_pos = select(pos_a, pos_b, use_b);"
        "\n    let pnmtxidx_a = load_pnmtxidx(vidx_a);"
        "\n    let pnmtxidx_b = load_pnmtxidx(vidx_b);"
        "\n    let in_pnmtxidx = select(pnmtxidx_a, pnmtxidx_b, use_b);"
        "\n    let mv_pos_a = vec4f(pos_a, 1.0) * ubuf.postex_mtx[pnmtxidx_a];"
        "\n    let mv_pos_b = vec4f(pos_b, 1.0) * ubuf.postex_mtx[pnmtxidx_b];"
        "\n    let mv_pos = select(mv_pos_a, mv_pos_b, use_b);"
        "\n    let clip_a = vec4f(mv_pos_a, 1.0) * ubuf.proj;"
        "\n    let clip_b = vec4f(mv_pos_b, 1.0) * ubuf.proj;"
        "\n    let ndc_a = clip_a.xy / clip_a.w;"
        "\n    let ndc_b = clip_b.xy / clip_b.w;"
        "\n    let viewport_scale = ubuf.render_viewport_size / max(ubuf.logical_viewport_size, vec2f(1.0));"
        "\n    let delta_px = (ndc_b - ndc_a) / 2.0 * ubuf.render_viewport_size;"
        "\n    let dir_px = select(vec2f(1.0, 0.0), normalize(delta_px), dot(delta_px, delta_px) > 1e-10);"
        "\n    let perp_px = vec2f(-dir_px.y, dir_px.x);"
        "\n    let line_width = ubuf.line.x * min(viewport_scale.x, viewport_scale.y);"
        "\n    let offset_px = perp_px * (line_width / 2.0) * select(-1.0, 1.0, (vidx & 1u) != 0u);"
        "\n    let offset_ndc = (offset_px * 2.0) / ubuf.render_viewport_size;"
        "\n    let clip_base = select(clip_a, clip_b, use_b);"
        "\n    out.pos = vec4f(clip_base.xy + offset_ndc * clip_base.w, clip_base.zw);",
        lineMode == 1 ? 2 : 1);
    // S runs across the line's width (see the specialized shaders)
    tcOffset =
        "\n        if ((line_mask & (1u << i)) != 0u && (vidx & 1u) != 0u) {"
        "\n            p.x += ubuf.line.z;"
        "\n        }";
  }
  replace_all(body, "%VTX_IN%", vtxIn);
  replace_all(body, "%VTX_PRE%", vtxPre);
  replace_all(body, "%TC_OFFSET%", tcOffset);

  // Outputs (as the specialized shaders): normal target, dual source blending, zfreeze depth.
  const bool normalTarget = normalAttachment != UINT32_MAX;
  replace_all(body, "%NRM_OUT_DECL%", normalTarget ? "\n    @location(10) mv_nrm: vec3f," : "");
  replace_all(body, "%NRM_OUT%", normalTarget ? "\n    out.mv_nrm = mv_nrm;" : "");
  std::string fragOutput;
  std::string fragOutType = "@location(0) vec4f";
  std::string fragReturn = "\n    return prev;";
  if (normalTarget) {
    fragOutput = fmt::format(
        "\nstruct FragmentOutput {{\n"
        "    @location(0) color: vec4f,\n"
        "    @location({}) normal: vec4f,\n"
        "}};\n",
        normalAttachment);
    fragOutType = "FragmentOutput";
    fragReturn =
        "\n    var out: FragmentOutput;"
        "\n    out.color = prev;"
        "\n    if ((fl & 0x80u) != 0u) {"
        "\n        let nrm_len_sq = dot(in.mv_nrm, in.mv_nrm);"
        "\n        let unit_nrm = select(vec3f(0.0), normalize(in.mv_nrm), nrm_len_sq > 1e-10);"
        "\n        out.normal = vec4f(unit_nrm * 0.5 + 0.5, select(0.0, 1.0, nrm_len_sq > 1e-10));"
        "\n    } else {"
        "\n        out.normal = vec4f(0.5, 0.5, 0.5, 0.0);"
        "\n    }"
        "\n    return out;";
  }
  if (dstAlphaMode == DstAlphaMode::DualSource) {
    fragOutput =
        "\nstruct FragmentOutput {\n"
        "    @location(0) @blend_src(0) color: vec4f,\n"
        "    @location(0) @blend_src(1) blend: vec4f,\n"
        "};\n";
    fragOutType = "FragmentOutput";
    fragReturn = "\n    return FragmentOutput(vec4f(prev.rgb, 1.0), prev);";
  }
  std::string fragPre;
  if (zFreeze) {
    fragPre = "\n    let zfreeze_depth = clamp(dot(ubuf.zfreeze_plane.xyz, vec3f(in.pos.xy, 1.0)), 0.0, 1.0);";
    constexpr std::string_view depthField = "    @builtin(frag_depth) depth: f32,\n";
    if (fragOutput.empty()) {
      fragOutput = fmt::format("\nstruct FragmentOutput {{\n    @location(0) color: vec4f,\n{}}};\n", depthField);
      fragOutType = "FragmentOutput";
      fragReturn = "\n    return FragmentOutput(prev, zfreeze_depth);";
    } else {
      fragOutput.insert(fragOutput.rfind("};"), depthField);
      if (dstAlphaMode == DstAlphaMode::DualSource) {
        fragReturn = "\n    return FragmentOutput(vec4f(prev.rgb, 1.0), prev, zfreeze_depth);";
      } else {
        fragReturn.insert(fragReturn.rfind("\n    return out;"), "\n    out.depth = zfreeze_depth;");
      }
    }
  }
  replace_all(body, "%FRAG_OUTPUT_STRUCT%", fragOutput);
  replace_all(body, "%FRAG_OUT_TYPE%", fragOutType);
  replace_all(body, "%FRAG_PRE%", fragPre);
  replace_all(body, "%FRAG_RETURN%", fragReturn);
  replace_all(body, "%DST_ALPHA%", dstAlphaMode == DstAlphaMode::Replace ? "\n    prev.a = 1.0;" : "");
  const std::string_view fogDepth = zFreeze ? (UseReversedZ ? "(1.0 - zfreeze_depth)" : "zfreeze_depth")
                                    : UseReversedZ ? "(1.0 - in.pos.z)"
                                                   : "in.pos.z";
  replace_all(body, "%FOG_DEPTH%", fogDepth);
  src += body;
  return src;
}

wgpu::ShaderModule build_uber_shader(const PipelineConfig& config, const gfx::RenderTargetLayout& layout,
                                     DstAlphaMode dstAlphaMode) noexcept {
  ZoneScoped;
  uint32_t normalAttachment = UINT32_MAX;
  for (uint32_t i = 0; i < layout.colorAttachmentCount; ++i) {
    if (layout.colorAttachments[i].semantic == gfx::ColorAttachmentSemantic::Normal) {
      normalAttachment = i;
    }
  }
  const auto source = build_uber_shader_source(config.shaderConfig.lineMode, config.shaderConfig.zFreeze,
                                               dstAlphaMode, normalAttachment);
  wgpu::ShaderSourceWGSL wgslDescriptor{};
  wgslDescriptor.code = source.c_str();
  const auto label = fmt::format("GX Ubershader {} {} {} {}", config.shaderConfig.lineMode,
                                 static_cast<u32>(config.shaderConfig.zFreeze), static_cast<u32>(dstAlphaMode),
                                 normalAttachment);
  const wgpu::ShaderModuleDescriptor shaderDescriptor{
      .nextInChain = &wgslDescriptor,
      .label = label.c_str(),
  };
  return webgpu::g_device.CreateShaderModule(&shaderDescriptor);
}
} // namespace aurora::gx
