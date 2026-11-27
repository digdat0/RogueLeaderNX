"""RL-NX: raise the Dawn Null backend's simulated memory cap.

Dawn's Null device exists for tests and validation, and it accounts for every buffer it is asked
for against a fixed limit (512 MiB, DeviceNull.h kMaxMemoryUsage). That limit is fine for a test
that draws a triangle; it is not fine for a whole game renderer, and this port uses the Null device
for real: deko3d is the actual renderer, so Dawn's device only has to carry pipeline objects,
staging buffers and the uber shader data - but that is still more than 512 MiB over a session, and
the limit shows up as `fatal: Aurora: WebGPU error 3: Out of memory` mid-boot.

The cap is a test heuristic, not a device property, so raising it changes nothing else.

Usage:  py -3 -I switch/tools/patch-dawn-null-memory.py ref/dawn
"""
import pathlib
import sys

OLD = "static constexpr uint64_t kMaxMemoryUsage = 512ULL * 1024 * 1024;"
NEW = "static constexpr uint64_t kMaxMemoryUsage = 8ULL * 1024 * 1024 * 1024;  // RL-NX: 8 GiB (was 512 MiB)"


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1])
        return 2
    path = pathlib.Path(sys.argv[1]) / "src/dawn/native/null/DeviceNull.h"
    if not path.is_file():
        print(f"missing {path}")
        return 1
    text = path.read_bytes().decode("utf-8")
    if NEW in text:
        print(f"{path.name}: already patched")
        return 0
    if text.count(OLD) != 1:
        print(f"{path.name}: expected exactly one {OLD!r}, found {text.count(OLD)}")
        return 1
    path.write_bytes(text.replace(OLD, NEW).encode("utf-8"))
    print(f"patched {path.name}: Null memory cap 512 MiB -> 8 GiB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
