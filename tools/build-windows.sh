#!/bin/bash
# Cross-compiles a RELEASE Windows build (engine.dll + SH.exe) from Linux,
# with llvm-mingw (https://github.com/mstorsjo/llvm-mingw) -- no Windows
# machine, Visual Studio or Vulkan SDK needed. build-all.bat is the native
# route; this is the one that runs here.
#
# Usage: tools/build-windows.sh            (run from the repo root)
#   LLVM_MINGW=/path/to/llvm-mingw-...     toolchain root (default below)
#
# Output: build/windows/ -- engine.dll, SH.exe, and the two llvm-mingw
# runtime DLLs they need (libc++.dll, libunwind.dll). Pair it with
# bin/assets via tools/package-windows.sh.
#
# Why the C++ runtime is a DLL: engine.dll and SH.exe pass std::string /
# std::vector across the DLL boundary, which is only safe when both sides
# share one libc++ (one heap, one set of type layouts) -- the same reason
# build.bat uses -fms-runtime-lib=dll.
set -euo pipefail

LLVM_MINGW=${LLVM_MINGW:-$HOME/.local/share/llvm-mingw-dl/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64}
TARGET=x86_64-w64-mingw32
CXX="$LLVM_MINGW/bin/$TARGET-clang++"
DLLTOOL="$LLVM_MINGW/bin/llvm-dlltool"
[ -x "$CXX" ] || { echo "llvm-mingw not found at $LLVM_MINGW (set LLVM_MINGW)"; exit 1; }

OUT=build/windows
OBJ=$OUT/obj
rm -rf "$OUT"
mkdir -p "$OBJ/engine" "$OBJ/SH" "$OUT/include" "$OUT/lib"

# Third-party headers: only Vulkan and glm. Symlinked individually so the
# cross compiler never sees the rest of /usr/include (glibc's headers).
for dir in vulkan vk_video glm; do
  [ -d "/usr/include/$dir" ] && ln -s "/usr/include/$dir" "$OUT/include/$dir"
done

# vulkan-1.dll's import library, generated from the Linux loader's export
# list: the Khronos loader exports the same core + WSI entry points on both
# platforms (minus each one's window-system surface functions, swapped
# below). Anything else the engine needs goes through vkGetInstanceProcAddr.
{
  echo "LIBRARY vulkan-1.dll"
  echo "EXPORTS"
  {
    nm -D --defined-only /usr/lib/libvulkan.so.1 | awk '{print $3}' | grep '^vk' |
      grep -vE 'Xlib|Xcb|Wayland|DirectFB|Screen|Metal|Android|Fuchsia|Ggp|ViSurface'
    echo "vkCreateWin32SurfaceKHR"
    echo "vkGetPhysicalDeviceWin32PresentationSupportKHR"
  } | sort -u
} > "$OUT/lib/vulkan-1.def"
"$DLLTOOL" -m i386:x86-64 -d "$OUT/lib/vulkan-1.def" -l "$OUT/lib/libvulkan-1.a"

COMMON=(-O2 -std=c++20 -fdeclspec -DNDEBUG -D_CRT_SECURE_NO_WARNINGS -isystem "$OUT/include")
JOBS=$(nproc)

# compile <module> <define> <include flags...> -- <sources...>
compile() {
  local module=$1 define=$2; shift 2
  local includes=()
  while [ "$1" != "--" ]; do includes+=("$1"); shift; done; shift
  printf '%s\n' "$@" | xargs -P "$JOBS" -I{} sh -c '
    src="$1"; shift; obj="$1"; shift
    "$@" -c "$src" -o "$obj/$(echo "$src" | tr / _).o" || exit 255' _ {} "$OBJ/$module" \
    "$CXX" "${COMMON[@]}" "$define" "${includes[@]}"
}

echo "Building engine.dll..."
mapfile -t engine_sources < <(find engine/src -name '*.cpp' | sort)
compile engine -DKEXPORT -Iengine/src -- "${engine_sources[@]}"
# --out-implib writes the import library SH.exe links against.
"$CXX" -shared -o "$OUT/engine.dll" "$OBJ"/engine/*.o \
  -Wl,--out-implib,"$OUT/lib/libengine.dll.a" \
  -L"$OUT/lib" -lvulkan-1 -luser32 -lwinmm

echo "Building SH.exe..."
mapfile -t sh_sources < <(find games/SH/src -name '*.cpp' | sort)
compile SH -DKIMPORT -Igames/SH/src -Iengine/src -- "${sh_sources[@]}"
# -mwindows: GUI subsystem, so double-clicking the game doesn't also open a
# console window. Log output still lands in console.log.
"$CXX" -mwindows -o "$OUT/SH.exe" "$OBJ"/SH/*.o -L"$OUT/lib" -lengine

cp "$LLVM_MINGW/$TARGET/bin/libc++.dll" "$LLVM_MINGW/$TARGET/bin/libunwind.dll" "$OUT/"
echo "Built $OUT/SH.exe"
