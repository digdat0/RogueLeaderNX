#!/usr/bin/env python3
"""Stage Aurora with the deko3d seams (RL_NX_DEKO=1).

    py -3 switch/builder/integrate-deko.py [--from build-switch/aurora-patched] [--to build-switch/aurora-deko]

switch/deko does not replace Aurora's renderer: Aurora keeps recording every frame as WebGPU, Dawn's
Null device keeps the object model, and switch/deko wraps Dawn's C entry points (wrap-symbols.txt) to
execute that recording on deko3d and to present the picture. So Aurora itself needs only three edits,
all marked RL-NX-DEKO:

  1. lib/webgpu/gpu.cpp, initialize(): create the native device before any WebGPU object exists.
  2. lib/webgpu/gpu.cpp, shutdown(): wait for the GPU and release the window.
  3. lib/gfx/frame.cpp, end_frame()'s render-worker job: present the game's finished frame texture,
     because the headless surface has no present target (`gfx::xfb_target()` stays Invalid, so
     `present::submit()` returns at its first line).

Every edit is a string replacement with an exact anchor: a missing anchor is a hard error naming the
file and the anchor, because a silently skipped seam is a black screen with no log line. The staged
tree is a full copy (aurora-deko), so aurora-patched and the desktop build are never touched.

This replaced the Foxhollow-shaped integration (40+ edits against `g_surface`, `depth_peek.cpp` and
the old `lib/aurora.cpp` acquire/present): none of those objects exist in this Aurora (docs
SWITCH_PORT.md §E 4b has the measured anchor census).
"""
from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_FROM = ROOT / "build-switch" / "aurora-patched"
DEFAULT_TO = ROOT / "build-switch" / "aurora-deko"

MARK = "RL-NX-DEKO"


class SeamError(RuntimeError):
    pass


def read(path: Path) -> tuple[str, bool]:
    """Text with \\n newlines, plus whether the file was CRLF (Aurora is CRLF in the tree)."""
    raw = path.read_bytes().decode("utf-8")
    return (raw.replace("\r\n", "\n"), True) if "\r\n" in raw else (raw, False)


def write(path: Path, text: str, crlf: bool) -> None:
    path.write_bytes((text.replace("\n", "\r\n") if crlf else text).encode("utf-8"))


def insert_after(path: Path, anchor: str, addition: str, what: str) -> None:
    text, crlf = read(path)
    if MARK in text and addition.strip() in text:
        print(f"  = {path.name}: {what} (already staged)")
        return
    count = text.count(anchor)
    if count != 1:
        raise SeamError(f"{path}: anchor for {what} occurs {count} times, expected 1:\n    {anchor!r}")
    text = text.replace(anchor, anchor + addition)
    write(path, text, crlf)
    print(f"  + {path.name}: {what}")


def replace_once(path: Path, old: str, new: str, what: str) -> None:
    text, crlf = read(path)
    if new in text:
        print(f"  = {path.name}: {what} (already staged)")
        return
    count = text.count(old)
    if count != 1:
        raise SeamError(f"{path}: anchor for {what} occurs {count} times, expected 1:\n    {old!r}")
    write(path, text.replace(old, new), crlf)
    print(f"  + {path.name}: {what}")


def stage(has: Path, out: Path) -> None:
    if not (has / "CMakeLists.txt").is_file():
        raise SeamError(f"{has} is not a staged Aurora tree (run switch/build.sh once without RL_NX_DEKO)")
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(has, out, symlinks=True, ignore=shutil.ignore_patterns(".git", "build*"))
    print(f"staged {out.relative_to(ROOT)} from {has.relative_to(ROOT)}")

    gpu = out / "lib" / "webgpu" / "gpu.cpp"
    frame = out / "lib" / "gfx" / "frame.cpp"

    # The deko entry points, and Aurora's own pipeline-config version (part of the DKSH compiler id).
    insert_after(
        gpu,
        "#include \"gpu.hpp\"\n",
        f"""#ifdef AURORA_GFX_DEKO3D
// {MARK}: the native renderer (switch/deko). Kartpad_backend.h takes the frame texture to present;
// dk_aurora.h owns the device, the swapchain on the default NWindow and the DKSH caches.
#include "kartpad_backend.h"
#include "dk_aurora.h"
#include "../gx/pipeline.hpp"
#endif
""",
        "deko includes",
    )

    insert_after(
        gpu,
        "bool initialize(AuroraBackend auroraBackend, bool allowCpu) {\n",
        f"""#ifdef AURORA_GFX_DEKO3D
  // {MARK}: the native device must exist before the first WebGPU object, because every object
  // Dawn creates from here on is shadowed by one on deko3d (switch/deko/aurora/dk_objects.cpp).
  aurora_switch_dk_init(gx::GXPipelineConfigVersion);
#endif
""",
        "aurora_switch_dk_init()",
    )

    insert_after(
        gpu,
        "void shutdown() {\n",
        f"""#ifdef AURORA_GFX_DEKO3D
  // {MARK}: wait for the GPU and hand the window back while the object model is still alive.
  aurora_switch_dk_shutdown();
#endif
""",
        "aurora_switch_dk_shutdown()",
    )

    # The picture. With AURORA_SURFACE_HEADLESS (the console has no SDL window) bind_xfb_surface()
    # creates no present target, so present::submit() returns at `if (!snapshot.state ...)`, and deko3d
    # blits the frame to the swapchain with its own present shader.
    #
    # RL-NX: what it blits is the composed XFB, as on PC (recording.cpp record_xfb_present ->
    # xfb_present_source()), NOT the EFB. Rogue Leader renders each frame in two EFB passes (top and
    # bottom halves, scissors 0,16 640x242 and 0,238 640x242) and display-copies each half into the
    # XFB with clear, so by the end of the frame the EFB holds only the strip neither copy cleared:
    # the "640x30 band" of README.PORT.md 8.1 was that strip, not a squashed picture.
    # The source is captured in the packet at end_recording() (recording thread), because g_xfb can be
    # replaced by the next frame's recording while this frame's present runs on the render worker.
    frame_packet = out / "lib" / "gfx" / "frame_packet.hpp"
    recording = out / "lib" / "gfx" / "recording.cpp"
    insert_after(
        frame_packet,
        "  std::vector<present::Request> presents;\n",
        f"""#ifdef AURORA_GFX_DEKO3D
  // {MARK}: the texture deko3d presents for this frame (the composed XFB, or the EFB when there is
  // none), captured at end_recording(); width/height are its size, for the log.
  wgpu::Texture dekoPresent;
  uint32_t dekoPresentWidth = 0;
  uint32_t dekoPresentHeight = 0;
#endif
""",
        "FramePacket::dekoPresent",
    )
    replace_once(
        recording,
        "  const RecordedFrame recorded{.packet = g_recorder.packet, .frameSlot = g_recorder.frameSlot};\n",
        f"""#ifdef AURORA_GFX_DEKO3D
  // {MARK}: what the deko3d present shows for this frame: the composed XFB when the game made one
  // (xfb_present_source() falls back to the EFB's present source otherwise, e.g. R2_NO_XFB=1).
  {{
    const auto source = xfb_present_source();
    frame.dekoPresent = source.texture;
    frame.dekoPresentWidth = source.size.width;
    frame.dekoPresentHeight = source.size.height;
  }}
#endif
  const RecordedFrame recorded{{.packet = g_recorder.packet, .frameSlot = g_recorder.frameSlot}};
""",
        "capture the deko present source",
    )
    replace_once(
        frame,
        "    for (const auto& request : packet.presents) {\n      present::submit(request);\n    }\n",
        f"""    for (const auto& request : packet.presents) {{
      present::submit(request);
    }}
#ifdef AURORA_GFX_DEKO3D
    // {MARK}: present the finished frame. packet.presents is empty in a headless build (there is no
    // surface to blit into), so the composed XFB captured at end_recording() goes straight to deko3d's
    // swapchain (stretched to the window, like Aurora's letterbox fill of a 450-line XFB).
    if (packet.dekoPresent) {{
      kartpad_deko_present(packet.dekoPresent.Get(), packet.dekoPresentWidth, packet.dekoPresentHeight);
    }} else if (webgpu::g_frameBuffer.texture) {{
      const auto size = gfx::efb::applied_size();
      kartpad_deko_present(webgpu::g_frameBuffer.texture.Get(), size.native_fb_width, size.native_fb_height);
    }}
#endif
""",
        "present the finished frame",
    )
    # RL-NX: thread placement on Horizon. Aurora names and places its threads in thread::set_current
    # (the game's "Main thread", "Aurora render worker", "Aurora FIFO processor"), but the placement
    # code there is Linux/Windows only, so on the console every thread stayed on the core it was
    # created on. switch/src/perf_switch.cpp's melee_nx_perf_thread maps the name to a role, applies
    # the perf.cfg policy (default "tuned": the game thread alone on core 1), logs a THREAD line and
    # registers the thread with the sampling profiler. Melee-NX's thread-sweep patch did this there.
    thread_cpp = out / "lib" / "thread.cpp"
    insert_after(
        thread_cpp,
        "namespace aurora::thread {\n",
        f"""#ifdef __SWITCH__
// {MARK}: switch/src/perf_switch.cpp - places a named thread on its core and registers it for profiling.
extern "C" void melee_nx_perf_thread(const char* name);
#endif
""",
        "declare melee_nx_perf_thread",
    )
    insert_after(
        thread_cpp,
        "  SDL_SetCurrentThreadPriority(to_sdl_priority(options.priority));\n",
        f"""#ifdef __SWITCH__
  // {MARK}: Horizon placement by name (unknown names are ignored there).
  if (!options.name.empty()) {{
    melee_nx_perf_thread(options.name.c_str());
  }}
#endif
""",
        "place Aurora threads on Horizon",
    )
    # efb::applied_size() needs its header in that translation unit.
    insert_after(
        frame,
        "#include \"../webgpu/gpu_prof.hpp\"\n",
        """#ifdef AURORA_GFX_DEKO3D
#include "efb.hpp"
#endif
#include "kartpad_backend.h"  // kartpad_deko_present
""",
        "efb.hpp + deko present include",
    )
    # The two asynchronous-readback compute pipelines (efb_gather.cpp and copy_pack.cpp) are the only
    # compute pipelines Aurora builds, and switch/deko's shader modules are *native* handles, not Dawn
    # objects - a compute pipeline would hand Dawn one and crash in PipelineBase's entry-point lookup
    # (the kit's "compiled out under deko"). The native renderer keeps its own EFB snapshot instead.
    readback = out / "lib" / "gfx" / "readback" / "gpu.cpp"
    replace_once(
        readback,
        "  initialize_efb_gather();\n  initialize_copy_pack();\n  set_supported(true);\n",
        f"""#ifdef AURORA_GFX_DEKO3D
  // {MARK}: the deko3d backend serves the game's CPU reads from its own EFB snapshot
  // (switch/deko/backend_present.cpp), so Aurora's compute gather and copy-pack are not built: their
  // shader modules would be native handles (switch/deko/backend_pipelines.cpp), which Dawn cannot turn
  // into a pipeline. Readback simply reports itself unavailable, as it already does without core
  // features or with MSAA.
  Log.info("Asynchronous readback: served by the deko3d backend");
  return;
#endif
  initialize_efb_gather();
  initialize_copy_pack();
  set_supported(true);
""",
        "skip Aurora's readback compute pipelines",
    )
    print("Aurora staged with the deko3d seams")


def main() -> int:
    parser = argparse.ArgumentParser(description="Stage Aurora with the deko3d seams")
    # build.sh passes --src/--dst; the longer spellings stay for hand runs.
    parser.add_argument("--src", "--from", dest="src", type=Path, default=DEFAULT_FROM)
    parser.add_argument("--dst", "--to", dest="dst", type=Path, default=DEFAULT_TO)
    args = parser.parse_args()
    try:
        stage(args.src.resolve(), args.dst.resolve())
    except SeamError as error:
        print(f"integrate-deko: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
