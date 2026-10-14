#!/bin/bash
# Packages the Linux build for itch.io: dist/SH-linux/ and
# dist/SH-linux.tar.gz -- the counterpart of tools/package-windows.sh, and
# ships the same subset of bin/assets (no beatsheets, shader sources, notes
# or the dev machine's saves; see that script).
#
# Usage: tools/package-linux.sh   (run from the repo root, after
#        tools/build-linux.sh and tools/bake-chunk-cache.sh)
set -euo pipefail

BUILD=build/linux
STAGE=dist/SH-linux
[ -x "$BUILD/SH" ] || { echo "No $BUILD/SH -- run tools/build-linux.sh first"; exit 1; }
if ! ls bin/assets/chunk_cache/*.brkz >/dev/null 2>&1; then
  echo "WARNING: no bin/assets/chunk_cache -- run tools/bake-chunk-cache.sh"
  echo "         first, or players wait for every scene to bake."
fi

rm -rf "$STAGE" dist/SH-linux.tar.gz
mkdir -p "$STAGE"
cp "$BUILD"/SH "$STAGE/"

tar -C bin -cf - \
  --exclude='*.docx' --exclude='*.glsl' --exclude='*.md' --exclude='.gitkeep' \
  assets | tar -C "$STAGE" -xf -

cat > "$STAGE/README.txt" <<'EOF'
SH
==

Run ./SH. Keep every file in this folder together.

Requirements
  - 64-bit Linux with X11 (or XWayland)
  - A GPU with an up-to-date Vulkan driver (Mesa, or NVIDIA's)
  - glibc 2.27 or newer (any distro from 2018 on)
  - The Vulkan loader, ALSA, mpg123 and the X11/xcb libraries -- installed
    on most desktops already (Debian/Ubuntu: libvulkan1 libasound2 libx11-6
    libx11-xcb1 libxcb1 libmpg123-0)

The first launch takes a little longer: the game compiles its shaders for
your graphics card, which can sit on a black screen for a while. Later
launches reuse them (~/.cache/kengine).

Your save (sh_save.txt) and settings (sh_settings.txt) are written to this
folder.
EOF

tar -C dist -czf dist/SH-linux.tar.gz SH-linux
du -sh "$STAGE" dist/SH-linux.tar.gz
