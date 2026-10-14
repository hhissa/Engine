#!/bin/bash
# Packages the Windows build for itch.io: dist/SH-windows/ (the folder to
# `butler push`) and dist/SH-windows.zip (the file to upload by hand).
#
# Usage: tools/package-windows.sh   (run from the repo root, after
#        tools/build-windows.sh)
#
# Ships the build plus bin/assets, minus what's only there for development:
#   *.docx            the beatsheets/scripts (the whole story, for us only)
#   *.glsl, *.inc.*   shader sources -- the engine loads only the .spv
#   *.md, .gitkeep    notes / placeholders
# and never the dev machine's own sh_save.txt / sh_settings.txt / console.log,
# so a player starts from a fresh game.
set -euo pipefail

BUILD=build/windows
STAGE=dist/SH-windows
[ -f "$BUILD/SH.exe" ] || { echo "No $BUILD/SH.exe -- run tools/build-windows.sh first"; exit 1; }

rm -rf "$STAGE" dist/SH-windows.zip
mkdir -p "$STAGE"
cp "$BUILD"/SH.exe "$BUILD"/engine.dll "$BUILD"/libc++.dll "$BUILD"/libunwind.dll "$STAGE/"

tar -C bin -cf - \
  --exclude='*.docx' --exclude='*.glsl' --exclude='*.md' --exclude='.gitkeep' \
  assets | tar -C "$STAGE" -xf -

cat > "$STAGE/README.txt" <<'EOF'
SH
==

Run SH.exe. Keep every file in this folder together.

Requirements
  - Windows 10 or 11, 64-bit
  - A GPU with an up-to-date Vulkan driver (NVIDIA, AMD or Intel)

The first launch is slow: the game compiles its shaders and builds each
scene's cache the first time it is shown, which can take a while with a
black screen. Later launches reuse that cache (%LOCALAPPDATA%\kengine).

Your save (sh_save.txt) and settings (sh_settings.txt) are written to this
folder. If something goes wrong, console.log here has the details.
EOF
sed -i 's/$/\r/' "$STAGE/README.txt"

python3 -I - "$STAGE" dist/SH-windows.zip <<'EOF'
import os, sys, zipfile
src, out = sys.argv[1], sys.argv[2]
root = os.path.dirname(src)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for dirpath, _, files in os.walk(src):
        for name in sorted(files):
            path = os.path.join(dirpath, name)
            z.write(path, os.path.relpath(path, root))
EOF
du -sh "$STAGE" dist/SH-windows.zip
