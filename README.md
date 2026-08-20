# Bingus Engine

<img width="1908" height="1017" alt="image" src="https://github.com/user-attachments/assets/e9ee41c4-f78c-4e02-bf50-fe565c990bda" />


A custom C++20 Vulkan engine with **no mesh pipeline at all**. There is no vertex buffer, no index buffer, no rasterizer for scene geometry. Every surface in every scene is a signed distance field, voxelized into a sparse brick hierarchy and raymarched per-pixel in compute shaders.

Roughly 40,000 lines of C++ and 7,500 lines of GLSL, written from scratch on top of raw Vulkan — including the memory subsystem, the streaming scheduler, the renderer, a Qt scene editor, and a narrative game built on the result.

---

## Screenshots

### SDF Scene Editor

*Add a screenshot or two of `tools/sdf_editor` in action here — e.g. a scene mid-edit with the gizmo visible, or the Lights tab.*

<!-- ![Editor screenshot](docs/screenshots/editor/editor-1.png) -->

### Games built with the engine

#### SH

*Add gameplay screenshots of `games/SH` here.*

<!-- ![SH screenshot](docs/screenshots/games/SH/sh-1.png) -->

---

## Why build it this way?

Triangle rasterization is a solved problem with excellent off-the-shelf implementations. Signed distance fields are not, and they buy properties that meshes make awkward:

- **Geometry is a formula, not data.** A scene is a list of primitives and boolean operations. A wall is nine floats. Scene files are human-readable and diff-able, and the "asset pipeline" is a text parser.
- **Smooth blending is native.** Two shapes joined with a smooth-union blend into each other analytically. There is no retopology step, no seam, no UV problem — the surface simply *is* the blended field.
- **Constructive subtraction is free.** Carving a doorway out of a wall is one operation, not a boolean mesh op that produces degenerate triangles.
- **Level of detail is a sampling rate.** The same field can be sampled at any resolution, so LOD is a question of how finely you voxelize, not of maintaining several versions of the same asset.

The cost is that everything else — visibility, shadows, ambient occlusion, global illumination — has to be rebuilt against a representation that no engine convention assumes. That trade is the whole point of the project: it is an exercise in building a coherent renderer where none of the usual answers apply.

The architectural touchstone is Media Molecule's *Dreams*, which shipped a commercial SDF/point-splatting renderer and demonstrated the key insight this engine leans on repeatedly: **a temporal filter changes what techniques are affordable.** Once you can rely on frames averaging together, you can trace one ambient-occlusion ray per pixel, splat deliberately undersampled shadow maps, and dither instead of supersampling.

---

## Design philosophy

These are the principles the codebase actually follows, with real examples from it. They are worth more than the feature list, because they are what the code looks like from the inside.

### 1. Comments explain *why*, and record what failed

The single most distinctive thing about this codebase is comment density and comment *content*. Comments here rarely restate the code. They record the reasoning, the alternative that was tried first, and the symptom that killed it.

A representative example, from the chunked field's cell-alias map:

> `AN AXIS WITH A COUNT OF ONE IS NOT REPEATED, whatever its cell spacing says. [...] Getting this wrong disabled deduplication completely, in a way that looked like the whole idea failing rather than a bug: DiegosOffice's repeated boxes are authored 20 x 1 x 20, so Y has a spacing of 1.0 but a count of 1. [...] Measured 0% of cells copied where the arithmetic predicts 90%.`

This is deliberate. In a system where a subtle bug manifests as *missing geometry* rather than a crash, the expensive resource is not writing the code — it is rediscovering why the code is shaped the way it is six weeks later. Several comments in the renderer exist specifically to stop a future reader (including the author) from "simplifying" something back into a bug that has already been paid for once.

### 2. Correct by default; risk is opt-in until it has been watched running

Features that can *silently remove geometry* — as opposed to merely running slowly — ship disabled behind an environment variable until they have been validated against real content.

Brick deduplication lived behind `KENGINE_CHUNK_DEDUP` for exactly this reason: a mis-keyed alias copies the wrong cell's voxels, which reads as corrupted or missing surface, with nothing logged. It was only promoted to on-by-default (with the flag inverted to `KENGINE_NO_CHUNK_DEDUP`) once its failure modes were understood and closed.

The same instinct shows up structurally: the newer chunked/streamed field and the older fixed-cube field **share no GPU buffer at all**, deliberately, so that an addressing bug in the newer path cannot corrupt the proven one that the editor depends on.

### 3. Measure — and know when a measurement is meaningless

The engine instruments itself heavily: per-pass GPU timestamps, bake cost counters, candidate-list fallback rates, deduplication hit rates, chunk residency per clip level.

The harder-won half of this principle is knowing when an instrument is lying. The async compute queue on this hardware is queue index 1 of the *graphics* family — the two queues share one hardware engine. A submission's timestamp delta is therefore **elapsed wall time including interleaved graphics work**, not that submission's own cost. Two separate attempts at adaptively pacing the chunk bake failed by closing a feedback loop around exactly that number: the estimate inflated, window sizes collapsed to their floor, and each additional submission paid the same fixed elapsed cost again. Both attempts are documented in the code so the third does not repeat them.

The resulting rule is written into the constants themselves: bound GPU work by a **count of near-uniform units**, which can be counted honestly, rather than by predicted time, which cannot be measured here.

### 4. Pure logic stays testable; Vulkan stays at the edges

`ChunkStreamingManager` decides *what should be resident, in what order, and when a slot may be reused* — and contains no Vulkan types whatsoever. That is a deliberate seam: the scheduling policy that is hardest to reason about is the part that can be unit-tested without standing up a GPU context, and it is (`tests/src/systems/chunk_streaming_manager_tests.cpp`).

The same split appears in the resource layer: `sdf_scene.h` and `conversation.h` are pure parsers with no knowledge of any particular game's systems.

### 5. Contracts that cannot be checked at compile time are checked at build time

A compute shader's workgroup shape and its push-constant layout are contracts with the engine that neither compiler validates. Get the workgroup shape wrong and the dispatch silently covers the wrong cells; drop one `int` from a push-constant block and every field after it shifts by four bytes. Both have happened, and both presented as "geometry is missing" with nothing logged.

So `post-build.sh` disassembles the *compiled SPIR-V* and asserts the workgroup shape the engine's dispatch math assumes:

```
Verifying compiled shader contracts...
  Builtin.ChunkVoxelize.comp.spv: LocalSize 8 8 1 OK
```

Checking the compiled module rather than the source is the point: what matters is what the GPU actually runs.

### 6. Never stall the frame on work the frame does not need

The chunk bake is expensive and unavoidable. What is avoidable is making the renderer wait for it. Chunk baking runs on a separate queue against a ring of command buffers and fences; a chunk is not published into the chunk table until its fence confirms the bake finished, so the sampler never reads a half-written chunk and the graphics queue never waits on one.

The cost of that decision is a genuinely concurrent system — cross-queue write-after-read hazards handled with timeline semaphores, ring-delayed slot reuse so a slot is never rewritten under a live submission, and double-buffered rebakes so an edited chunk keeps serving its old content until the new content is ready. Much of the renderer's complexity is this, and the comments say so.

---

## How a frame is rendered

Every pass is a compute shader. There is no render pass for scene geometry.

| # | Pass | What it does |
|---|------|--------------|
| 1 | **Prepass: clears** | Resets the per-frame visibility and tile buffers |
| 2 | **Prepass: cluster cull** | Culls splat point clusters against the frustum, writing indirect dispatch args |
| 3 | **Prepass: voxel cascade** | Rebuilds binary occupancy cascades centred on the camera, snapped to their own grid |
| 4 | **Prepass: shadow splat** | Splats cluster points into 64 imperfect shadow maps, octahedral, one per light |
| 5 | **Prepass: point splat** | Splats surface points into a screen-space visibility buffer (64-bit depth+id, `atomicMin`) |
| 6 | **Visibility / G-buffer** | Resolves each pixel to a hit: splat-refined, or sphere-traced through the brick field |
| 7 | **Ambient occlusion** | One cosine-weighted ray per pixel — screen-space near, voxel cascades far |
| 8 | **Deferred shade** | Direct lighting, ISM shadows, GI cascade lookup, triplanar texturing, volumetrics |
| 9 | **TAA resolve** | Reprojects and blends history, with variance clipping and depth-based rejection |
| 10 | **Bloom** | Bright-pass and separable blur at half resolution |
| 11 | **Post composite** | Tonemap, pixelation, UI composite |

The split at step 6 is a G-buffer, and it exists so that shading is paid once per pixel instead of once per ray step.

---

## The SDF field: authoring → baking → sampling

**Authoring.** A scene is a list of primitives (15 types: sphere, box, plane, torus, capped cylinder/cone, round box, box frame, octahedron, pyramid, hex prism, round cone, capsule, link, ellipsoid) organised into layers. Each layer is a union or a subtraction with its own smoothness. Primitives carry rotation, domain repetition (limited, rectangular, infinite, rotational), materials, and triplanar textures.

Parameters can be **formulas rather than constants** — a limb's radius can taper as a function of height — evaluated by a small expression VM (`resources/expression.h`) that runs on both CPU and GPU.

**Baking.** Evaluating the full analytic scene per ray step is far too slow, so the scene is voxelized into a sparse brick field:

- Space is divided into **chunks** of 16³ coarse cells.
- A cell that the surface passes through is allocated a **brick** — an 18³ grid of distances (16³ plus a one-voxel apron on every side, so trilinear sampling across brick boundaries needs no cross-brick communication).
- Bricks live in a shared pool with a GPU free list; an **indirection table** maps cell → brick; a **toroidal chunk table** maps world chunk coordinate → slot.

Two optimisations make this affordable, and both are about *not evaluating the scene*:

- **Per-chunk candidate lists.** For each chunk, the CPU works out which primitives can possibly reach it — resolving domain repetition down to the individual copies in range rather than folding the whole tiling. A sample then folds a handful of primitives instead of the entire scene.
- **Sub-block rejection.** A brick is walked as 3³ sub-blocks; one evaluation at a sub-block's centre, compared against its circumradius, can fill every voxel in it arithmetically. An SDF changes by at most the distance travelled, so the fill is a conservative under-estimate — always safe in the direction that matters.
- **A persistent brick cache.** A baked chunk is gathered into one contiguous quantized blob on the GPU, read back, and written to disk under a hash of the scene content that reaches it. Re-entering that chunk — later in the session, or in a completely different run — uploads the blob and scatters it back into freshly allocated bricks, skipping the bake entirely. Because the key is content-derived rather than a scene version counter, moving one primitive invalidates only the chunks near it while every other chunk in the scene still hits.
- **Brick deduplication.** Repeated architecture means most cells bake identical bricks. Each cell is keyed by *the function the voxelizer would fold there* — candidate identity plus this cell's position relative to each, reduced modulo the repetition period. Matching keys copy a brick instead of evaluating ~2,000 scene samples.

**Pre-baking.** A cold bake is on the order of ten milliseconds of GPU work; restoring that same chunk from the cache is a fraction of one. That gap is worth paying at load time rather than during play, so the whole scene is baked into the cache up front whenever a cache directory is configured (opt out with `KENGINE_NO_PREWARM_CACHE`).

Doing that well means finding the chunks that contain surface without visiting the ones that do not — and a bounding volume cannot answer that question. An unbounded primitive (a plane, an infinite repetition, a parameter driven by a formula) reaches every chunk in the world, so every chunk has a candidate and nothing can be ruled out by candidacy alone. Enumerating one room-sized scene's bounding box across all five clip levels came to 316,329 chunks, not one of them provably empty.

So the pre-bake is **hierarchical rather than volumetric**. It walks the clip levels coarsest-first, bakes a level, asks each chunk how many bricks it actually allocated, and descends only into the eight children of the ones that found something. Empty space is rejected in 64-unit blocks at the top instead of being visited 4-unit chunk by 4-unit chunk, so the cost tracks the scene's surface *area* rather than its volume: the same scene resolves to under a thousand chunks visited, a few hundred of which hold surface and are written to disk. Chunks with no surface are deliberately never written — they cost almost nothing to bake at runtime, so a file would buy nothing and hundreds of thousands of them would bury the ones that matter.

The descent is safe because the occupancy test is the bake's own, and it works at *cell* granularity: a cell takes a brick when its centre is within half a cell diagonal of the surface, independent of that level's voxel resolution. A thin wall is therefore still found by a coarse ancestor that could not render it well.

**Sampling.** A ray sphere-traces the brick field, trilinearly interpolating within bricks and skipping analytically across empty cells and empty chunks (a ray-box slab exit, not a fixed step).

---

## Streaming and level of detail

The field is a **clipmap**: five levels, each with double the previous level's chunk size, each keeping a small window of chunks resident around the camera. A sample picks the finest level that is **actually resident** there — not merely the finest whose window contains it. Those are different things whenever a chunk is still baking, and conflating them is what turns "not ready yet" into "nothing here": the ray sails through geometry that exists and the surface renders as a chunk-shaped hole. Skipping a non-resident level hands the space to the coarser one that already covers it, so detail pops in for a frame or two instead. Both the marched field and the point-splat pass have to agree on this, since they describe the same surface to the same pixels.

The streaming scheduler handles the parts that are easy to get subtly wrong:

- **Velocity-biased prefetch** — the window is centred slightly ahead of the camera, along a smoothed velocity, so a chunk starts baking before it is needed.
- **Eviction hysteresis** — the evict window is wider than the load window, so a camera dithering across a chunk boundary does not evict-and-reload every frame.
- **Lazy eviction** — a chunk that leaves the window stays resident as long as slots are plentiful, because eviction is nearly free but a reload is a full bake. Backtracking finds the chunk still loaded.
- **Ring-delayed slot reuse** — a slot is not handed out again until enough frames have passed that no in-flight submission can still be writing it.

---

## Lighting

- **Global illumination**: multi-bounce baked probe grids, including a camera-centred cascade for the streamed field that recenters in discrete whole-cell steps.
- **Direct lighting**: point and directional lights, with lights synthesized automatically from emissive primitives so a glowing panel actually illuminates the room.
- **Shadows**: sphere-traced soft shadows for hero lights; **imperfect shadow maps** for the rest — 64 octahedral maps splatted from the same point cloud the visibility pass uses, deliberately low quality because a temporal filter cleans them up.
- **Ambient occlusion**: one cosine-weighted ray per pixel, traced against the depth buffer at contact scale and binary voxel cascades beyond, then spatially filtered with depth/normal rejection before TAA averages it.
- **Volumetrics**: light shafts as marched, textured, scrolling density volumes.

---

## Tooling

- **`tools/sdf_editor`** — a Qt6 visual scene editor that embeds the engine itself for its viewport, so what you author is rendered by the same code that runs the game. Scene tree, per-primitive property panels, transform gizmo, light editing, live re-bake on edit.
- **`tools/conversation_editor`** — an editor for the `.conversation` dialogue-tree format.
- **`games/SH`** — a narrative game built on the engine: chapter structure, menu system, save/load, an interrogation-style question/answer system driven by the conversation format, and free-fly debug cameras.

---

## Testing

A small custom test harness (`tests/`) covering the parts where correctness is subtle and GPU-independent:

- Chunk streaming scheduling — residency, eviction hysteresis, ring-delayed slot reuse, lazy eviction under slot pressure.
- Geometry/chunk coverage math — which chunks a primitive touches.
- The linear allocator.

Alongside these, the renderer carries a set of `debug_verify_*` harnesses that drive the real GPU path synchronously and read results back — used to verify that the chunked field agrees with the analytic scene at sample points, that multi-level selection picks the level it should, and that LOD blending behaves.

---

## Project layout

```
engine/          the engine (core, memory, platform, renderer, resources, systems)
  src/renderer/vulkan/     Vulkan backend; the raymarch shader driver is the largest file
  src/systems/             streaming scheduler, geometry/material/texture/shader systems
  src/resources/           .sdf and .conversation parsers, expression VM, image/font loading
assets/shaders/  GLSL compute/graphics shaders (compiled to SPIR-V by post-build)
assets/scenes/   .sdf scenes
testbed/         sandbox for exercising engine features
games/SH/        a narrative game built on the engine
tools/           Qt editors (sdf_editor, conversation_editor)
tests/           unit tests
bin/             build output; run targets from here
```

---

## Building

### Requirements

- A C++20 compiler (`clang++`; the build scripts invoke it directly)
- [Vulkan SDK](https://vulkan.lunarg.com/) with `glslc` on `PATH` or `$VULKAN_SDK` set
- `glm`
- X11 development libraries: `xcb`, `X11`, `X11-xcb`, `xkbcommon`
- Qt6 (`Widgets`, `Gui`) — only for the editors
- `spirv-dis` (optional) — enables the compiled-shader contract checks

### Build

From the repo root:

```bash
./build-all.sh    # engine (libengine.so), testbed, games, tests
./post-build.sh   # compiles every shader to SPIR-V and copies assets/ into bin/
```

(`build-all.bat` / `post-build.bat` on Windows.)

The engine builds as a shared library and the executables link against it with an `$ORIGIN` rpath, so everything runs out of `bin/` without installing anything.

### Run

Working directory matters — asset paths are relative to it:

```bash
cd bin
./testbed     # feature sandbox
./SH          # the game
./tests       # unit tests
```

### The editors

Separate CMake projects:

```bash
cd tools/sdf_editor
cmake -B build && cmake --build build
cd ../../bin
../tools/sdf_editor/build/sdf_editor
```

### Diagnostic environment variables

| Variable | Effect |
|---|---|
| `KENGINE_BAKE_STATS` | Tally per-bake cost counters (scene evaluations, voxels filled vs copied) and report them |
| `KENGINE_NO_CHUNK_DEDUP` | Disable brick deduplication (on by default) |
| `KENGINE_CHUNK_CACHE_DIR` | Directory for the persistent brick cache. Unset disables it — the engine will not pick a place to write files on its host's behalf. Entries are content-addressed, so a stale one can never be read back as valid. |
| `KENGINE_NO_PREWARM_CACHE` | Disable pre-baking the scene into the brick cache at load (on by default whenever `KENGINE_CHUNK_CACHE_DIR` is set). Pre-baking blocks while it runs, reports what it found per clip level, and re-arms on every scene load — but not on an edit, which must not stall the editor |
| `KENGINE_PREWARM_MAX_CHUNKS` | Budget for the above (default 20000). Measured against chunks that actually need baking, and it aborts with a count rather than blocking on an unexpectedly large scene |

---

## Known limitations and current work

Stated plainly, because the interesting engineering is here rather than in the feature list.

- **Chunk bake cost.** A dense chunk costs on the order of 200 ms of GPU time to voxelize, and the "async" compute queue shares a hardware engine with graphics on the target GPU, so that work competes with the frame. Crossing chunk boundaries in large scenes can therefore hitch.

  Three approaches to *scheduling* that work differently have been built and rejected — Z-row bake slicing, a GPU work-list with time-bounded windows, and GigaVoxels-style ray-guided population. The first two failed for the same reason: they re-partitioned the same work instead of reducing it, and both closed a feedback loop around a timestamp that does not measure what it appears to. Those attempts are documented in the code, which is more useful than quietly deleting them.

  The current direction is to reduce the work rather than reschedule it: brick deduplication and a persistent disk-backed brick cache (both landed), and interval-arithmetic tape pruning à la Keeter's *Massively Parallel Rendering of Complex Closed-Form Implicit Surfaces* (not yet started), which prunes the primitive expression hierarchically per region and is the only remaining idea with order-of-magnitude headroom.

- **Brick pool capacity** is the binding constraint on how much of a scene can stay resident. Narrow-band quantized storage would cut it several-fold and has not been done.

- **Single-platform.** The platform layer is Linux/X11; Windows build scripts exist but are not exercised.

---

## A note on scope

This is a solo project, and its value as a portfolio piece is not that it is finished — it is that it is a system with genuinely hard, coupled problems that were reasoned about in the open: GPU/CPU concurrency without stalls, a streaming scheduler with real invariants, a rendering technique with no off-the-shelf answers, and a measurement problem where the obvious instrument was actively misleading. The commentary throughout the code is the record of that reasoning, and is the thing I would most want a reviewer to read.
