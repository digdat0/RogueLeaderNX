"""RL-NX: start Aurora's controllers on the Switch.

Aurora initialises SDL's joystick, gamepad, haptic and sensor subsystems in aurora::update() on the
first frame, but only when the surface is not AURORA_SURFACE_HEADLESS. The console build is headless
(deko3d owns the window, there is no SDL window), so no subsystem was started: SDL's Switch joystick
driver never opened a pad, Aurora's PAD saw no controller, and the game sat on the title screen
ignoring every button (hardware, build 08:38, 2026-11-23). SDL_Init(SDL_INIT_JOYSTICK) also brings up
SDL_INIT_EVENTS, so window::poll_events() then pumps the gamepad-added events Aurora's pad layer needs.

Usage:  py -3 -I switch/tools/patch-aurora-switch-input.py build-switch/aurora-patched

String match (the vendored tree is CRLF, see patch-aurora-dawn-status.py); idempotent.
"""
import pathlib
import sys

OLD = "    if (window::surface_type() != AURORA_SURFACE_HEADLESS) {\n"
NEW = (
    "    // RL-NX: on the Switch the surface is headless but the controllers are real: start them anyway\n"
    "    // (switch/tools/patch-aurora-switch-input.py).\n"
    "#ifdef __SWITCH__\n"
    "    if (true) {\n"
    "#else\n"
    "    if (window::surface_type() != AURORA_SURFACE_HEADLESS) {\n"
    "#endif\n"
)


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-3])
        return 2
    path = pathlib.Path(sys.argv[1]) / "lib/aurora.cpp"
    if not path.is_file():
        print(f"missing {path}")
        return 1
    raw = path.read_bytes().decode("utf-8")
    crlf = "\r\n" in raw
    text = raw.replace("\r\n", "\n")
    if NEW in text:
        print(f"{path.name}: already patched (Switch input)")
        return 0
    if text.count(OLD) != 1:
        print(f"{path}: expected exactly one {OLD!r}, found {text.count(OLD)}")
        return 1
    text = text.replace(OLD, NEW)
    path.write_bytes((text.replace("\n", "\r\n") if crlf else text).encode("utf-8"))
    print(f"patched {path.name} (Switch input)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
