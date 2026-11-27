#!/usr/bin/env bash
# Packs a Linux build (tools/build_linux.sh) into an AppImage for the Steam Deck and other
# desktops:
#   tools/make_appimage.sh --runtime <runtime-x86_64> [--build-dir build/linux] [--icon <png>] [--out <file>]
# --icon: a 256x256 PNG (Red Two's own: res/red_two.png scaled down).
# --runtime: the AppImage type 2 runtime (github.com/AppImage/type2-runtime, release
# "continuous", runtime-x86_64); the squashfs image goes after it. Needs mksquashfs and patchelf.
# The AppImage holds the recompiled game: it is for the person whose disc it was built from.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build/linux"
runtime=""
icon=""
out=""
while [ $# -gt 0 ]; do
    case "$1" in
    --runtime) runtime="$(realpath "$2")"; shift 2 ;;
    --build-dir) build="$(realpath "$2")"; shift 2 ;;
    --icon) icon="$(realpath "$2")"; shift 2 ;;
    --out) out="$(realpath -m "$2")"; shift 2 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
done
[ -n "$runtime" ] || { echo "--runtime <runtime-x86_64> is required" >&2; exit 2; }
[ -n "$out" ] || out="$build/RedTwo-x86_64.AppImage"
exe="$build/red_two"
[ -x "$exe" ] || { echo "no $exe: build first (tools/build_linux.sh)" >&2; exit 1; }

app="$build/AppDir"
rm -rf "$app"
mkdir -p "$app/usr/bin" "$app/usr/lib" "$app/usr/share/red-two" "$app/usr/share/applications" \
    "$app/usr/share/icons/hicolor/256x256/apps"
# the program, without its DWARF (the dynamic symbols stay: crash call stacks)
objcopy --strip-debug "$exe" "$app/usr/bin/red_two"
patchelf --set-rpath '$ORIGIN/../lib' "$app/usr/bin/red_two"

# the libraries built here (SDL3, Dawn, ... when shared) and the system's that a desktop may lack
# or have in another version (libpng, FreeType and its compressors); not the C library, libm,
# zlib or anything graphics drivers bring
ldd "$exe" | awk '/=>/ {print $3}' | while read -r lib; do
    case "$lib" in
    "$build"/* | */libpng16.so* | */libfreetype.so* | */libbz2.so* | */libbrotlidec.so* | */libbrotlicommon.so*)
        name="$(basename "$lib")"
        cp -L "$lib" "$app/usr/lib/$name"
        objcopy --strip-debug "$app/usr/lib/$name" 2>/dev/null || true
        patchelf --set-rpath '$ORIGIN' "$app/usr/lib/$name" 2>/dev/null || true
        echo "bundled $name"
        ;;
    esac
done

# the menu's font (Windows has Bahnschrift; a condensed DejaVu - free license - here)
for f in /usr/share/fonts/truetype/dejavu/DejaVuSansCondensed.ttf /usr/share/fonts/TTF/DejaVuSansCondensed.ttf \
         /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf; do
    if [ -f "$f" ]; then
        cp "$f" "$app/usr/share/red-two/font.ttf"
        cp /usr/share/fonts/truetype/dejavu/LICENSE "$app/usr/share/red-two/font-LICENSE" 2>/dev/null ||
            cp /usr/share/doc/fonts-dejavu-core/copyright "$app/usr/share/red-two/font-LICENSE" 2>/dev/null || true
        break
    fi
done
cp "$root/LICENSE" "$app/usr/share/red-two/LICENSE"
# the starter shader list (tools/make_shader_seed.py): Aurora reads it next to the program
if [ -f "$root/build/initial_pipeline_cache.db" ]; then
    cp "$root/build/initial_pipeline_cache.db" "$app/usr/bin/initial_pipeline_cache.db"
else
    echo "note: no build/initial_pipeline_cache.db: no starter shader list (tools/make_shader_seed.py)"
fi
python3 "$root/tools/third_party_notices.py" "$build" "$app/usr/share/red-two/THIRD_PARTY_NOTICES.txt"
# the system libraries bundled above: their Debian copyright files
for f in /usr/share/doc/libpng16-16*/copyright /usr/share/doc/libfreetype6/copyright /usr/share/doc/libbz2-1.0/copyright \
         /usr/share/doc/libbrotli1/copyright; do
    [ -f "$f" ] || continue
    printf '\n%s\n%s (bundled from the build system)\n%s\n%s\n' "$(printf '=%.0s' $(seq 78))" "$(basename "$(dirname "$f")")" \
        "$(printf '=%.0s' $(seq 78))" "$(cat "$f")" >> "$app/usr/share/red-two/THIRD_PARTY_NOTICES.txt"
done

cat > "$app/AppRun" <<'EOF'
#!/bin/sh
# Red Two: your own Rogue Leader disc image (GSWE64 .iso / .rvz) next to this AppImage, in
# ~/Emulation/roms/gc, in ~/.local/share/Red Two/, or given with --disc <path>.
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/usr/bin/red_two" "$@"
EOF
chmod +x "$app/AppRun"

cat > "$app/usr/share/applications/red-two.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Red Two
GenericName=Star Wars: Rogue Squadron II
Comment=Star Wars: Rogue Squadron II - Rogue Leader, recompiled for PC (your own disc image)
Exec=red_two
Icon=red-two
Categories=Game;ActionGame;
Terminal=false
EOF
cp "$app/usr/share/applications/red-two.desktop" "$app/red-two.desktop"
if [ -n "$icon" ]; then
    cp "$icon" "$app/usr/share/icons/hicolor/256x256/apps/red-two.png"
    cp "$icon" "$app/red-two.png"
    ln -s red-two.png "$app/.DirIcon"
else
    echo "note: no --icon: the AppImage has no icon" >&2
fi

sq="$build/AppDir.squashfs"
rm -f "$sq"
mksquashfs "$app" "$sq" -root-owned -noappend -comp zstd -Xcompression-level 19 -b 1M -quiet
cat "$runtime" "$sq" > "$out"
chmod +x "$out"
rm -f "$sq"
echo "$out: $(du -h "$out" | cut -f1)"
