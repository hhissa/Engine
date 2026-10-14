#!/bin/bash
# Bakes every SH scene ahead of time into bin/assets/chunk_cache -- the
# read-only "seed" chunk cache the game ships with, so a player's first
# visit to each room restores it from disk instead of baking it on their
# GPU (see KENGINE_CHUNK_CACHE_SEED_DIR in vulkan_raymarch_shader.cpp).
#
# Usage: tools/bake-chunk-cache.sh   (run from the repo root, after
#        building the Linux engine and game into bin/)
#
# Runs bin/SH with SH_BAKE_CACHE, which tours every room and cutaway (the
# Preparing screen) into an empty cache and quits, then compresses each
# entry: the 8-byte header (format version, brick count) stays plain so the
# engine can check an entry without inflating it, and the payload becomes
# one zlib stream -- it is mostly saturated far-from-surface voxels and
# shrinks ~80x.
#
# Re-run after ANY scene, material or chunk-format change. A stale seed is
# never wrong -- entries are keyed by content, so a changed chunk simply
# misses and bakes on the player's machine -- but it stops saving the wait.
set -euo pipefail

[ -x bin/SH ] || { echo "No bin/SH -- build the engine and game first"; exit 1; }

RAW=$(pwd)/build/chunk_cache_raw
SEED=bin/assets/chunk_cache
rm -rf "$RAW"
mkdir -p "$RAW"

echo "Baking every scene into $RAW (opens the game window; takes a while)..."
(cd bin && SH_BAKE_CACHE="$RAW" ./SH) | grep -E 'SH_BAKE_CACHE|pre-warm complete|\[ERROR\]|\[FATAL\]' | grep -v 'A test message' || true

python3 -I - "$RAW" "$SEED" <<'EOF'
import os, sys, zlib
raw, seed = sys.argv[1], sys.argv[2]
names = sorted(n for n in os.listdir(raw) if n.endswith(".brk"))
if not names:
    sys.exit("The bake produced no cache entries -- check the game's output.")
os.makedirs(seed, exist_ok=True)
keep = set()
before = after = 0
for name in names:
    data = open(os.path.join(raw, name), "rb").read()
    out = name[:-len(".brk")] + ".brkz"
    keep.add(out)
    packed = data[:8] + zlib.compress(data[8:], 9)
    with open(os.path.join(seed, out), "wb") as fh:
        fh.write(packed)
    before += len(data)
    after += len(packed)
# The keys the bake found empty (see known_empty_chunks_ in the engine):
# kept as-is, so the player's pre-warm can skip re-baking them.
empty = os.path.join(raw, "empty.keys")
if os.path.exists(empty):
    data = open(empty, "rb").read()
    open(os.path.join(seed, "empty.keys"), "wb").write(data)
    keep.add("empty.keys")
    print(f"{len(data) // 8} empty chunks recorded")
# Entries for scenes that have since changed would never be read again.
stale = [n for n in os.listdir(seed) if n not in keep]
for n in stale:
    os.remove(os.path.join(seed, n))
print(f"{len(names)} chunks: {before / 1e6:.0f} MB baked -> "
      f"{after / 1e6:.1f} MB in {seed} ({len(stale)} stale entries removed)")
EOF
