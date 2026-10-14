#!/bin/bash
# Builds a RELEASE Linux build of SH that runs on any x86-64 distro from
# glibc 2.31 on (Ubuntu 20.04, Debian 11, Fedora 32 and everything newer)
# -- the counterpart of tools/build-windows.sh. engine/build.sh and
# games/SH/build.sh stay the development builds: they define _DEBUG, which
# makes the Vulkan validation layer REQUIRED at startup (vulkan_backend.cpp).
#
# Usage: tools/build-linux.sh   (run from the repo root)
#   ZIG="python -m ziglang"     override the toolchain (default below)
#
# Output: build/linux/SH -- one executable. Pair it with bin/assets via
# tools/package-linux.sh.
#
# Why zig, and why one executable:
#   - Built with this machine's own toolchain, the binary needs whatever
#     glibc and libstdc++ this (rolling) distro has: GLIBC_2.43 for atan2f
#     etc. and GCC 16's GLIBCXX_3.4.35 -- newer than almost every player's.
#     zig links against glibc 2.31's symbol versions instead, and links its
#     libc++ statically, so the only dynamic dependencies left are the
#     system libraries every desktop has (Vulkan, X11/xcb, ALSA, mpg123).
#   - A statically linked C++ runtime must not be split across two binaries
#     that pass std::string/std::vector between them (two heaps' worth of
#     bookkeeping, two copies of every type's identity), so the engine and
#     the game are linked into one executable here instead of SH +
#     libengine.so. Without KEXPORT/KIMPORT, KAPI is empty.
set -euo pipefail

if [ -z "${ZIG:-}" ]; then
  [ -x build/zigenv/bin/python ] || {
    echo "Installing zig into build/zigenv..."
    python3 -m venv build/zigenv && build/zigenv/bin/pip install -q ziglang
  }
  ZIG="build/zigenv/bin/python -m ziglang"
fi
export ZIG
TARGET=x86_64-linux-gnu.2.31

OUT=build/linux
OBJ=$OUT/obj
rm -rf "$OUT"
mkdir -p "$OBJ" "$OUT/include" "$OUT/lib"

# Only the third-party headers the engine uses, symlinked one by one so the
# compiler never sees the rest of /usr/include -- glibc's own headers here
# are 2.43's, and would pull in symbols 2.31 doesn't have (__isoc23_strtol).
for h in vulkan vk_video glm X11 xcb xkbcommon alsa mpg123.h fmt123.h; do
  ln -s "/usr/include/$h" "$OUT/include/$h"
done
# The system libraries, linked by soname -- the player's own copies are
# used at run time. These are this machine's, built against its newer
# glibc, so the link passes --allow-shlib-undefined: what THEY need from
# glibc is their business at run time (the player's copies match the
# player's glibc); only SH's own references are pinned to 2.31.
for l in vulkan xcb X11 X11-xcb xkbcommon asound mpg123; do
  so=$(ls /usr/lib/lib$l.so.[0-9]* 2>/dev/null | sort -V | head -1)
  [ -n "$so" ] || { echo "lib$l not found in /usr/lib"; exit 1; }
  ln -s "$so" "$OUT/lib/lib$l.so"
done

# -g1: line tables only, kept in symbols/ for reading crash backtraces.
COMMON=(-target "$TARGET" -O2 -g1 -std=c++20 -fdeclspec -DNDEBUG
        -isystem "$OUT/include")

echo "Compiling..."
mapfile -t sources < <(find engine/src games/SH/src -name '*.cpp' | sort)
printf '%s\n' "${sources[@]}" | xargs -P "$(nproc)" -I{} sh -c '
  src="$1"; shift; obj="$1"; shift
  $ZIG c++ "$@" -c "$src" -o "$obj/$(echo "$src" | tr / _).o" || exit 255' _ {} "$OBJ" \
  "${COMMON[@]}" -Iengine/src -Igames/SH/src

echo "Linking SH..."
$ZIG c++ -target "$TARGET" -o "$OUT/SH" "$OBJ"/*.o -L"$OUT/lib" \
  -Wl,--allow-shlib-undefined \
  -lvulkan -lxcb -lX11 -lX11-xcb -lxkbcommon -lasound -lmpg123

mkdir -p "$OUT/symbols"
cp "$OUT/SH" "$OUT/symbols/SH"
strip --strip-debug "$OUT/SH"
echo "Built $OUT/SH"
