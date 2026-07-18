# Bingus Engine

A custom Vulkan engine built around signed-distance-field (SDF) raymarching instead of a traditional mesh/rasterization pipeline. Scenes are authored as procedural primitives (spheres, boxes, capsules, and more) combined with smooth unions/subtractions, voxelized into a sparse brick field, and raymarched per-pixel in a compute shader — with a baked, multi-bounce GI probe grid for indirect lighting.

## Screenshots

### SDF Scene Editor

*Add a screenshot or two of `tools/sdf_editor` in action here — e.g. a scene mid-edit with the gizmo visible, or the Lights tab.*

<!-- ![Editor screenshot](docs/screenshots/editor/editor-1.png) -->

### Games built with the engine

#### SH

*Add gameplay screenshots of `games/SH` here.*

<!-- ![SH screenshot](docs/screenshots/games/SH/sh-1.png) -->

<!--
Building a new game with the engine? Copy this pattern:

#### YourGameName

*Add screenshots here.*

<!- ![YourGameName screenshot](docs/screenshots/games/YourGameName/screenshot-1.png) ->
-->

## Features

- Sparse voxel SDF raymarching — no mesh/rasterization path, every surface is procedural
- Primitive library: sphere, box, plane, torus, capped cylinder/cone, round box, box frame, octahedron, pyramid, hex prism, round cone, capsule, link, ellipsoid
- Layered smooth union/subtraction blending, with per-layer smoothness
- Parametric attributes — a primitive's parameters can be authored as formulas (e.g. taper a limb's radius by height) instead of fixed constants
- Baked multi-bounce indirect lighting via a GI probe grid, plus directional/point lights
- Triplanar texturing with procedural bump mapping derived from diffuse texture luminance
- `.sdf` scene file format, plus a standalone Qt visual editor (`tools/sdf_editor`) for authoring scenes without hand-writing files
- Free-fly debug camera support for games (see `games/SH`)

## Project layout

- `engine/` — the engine itself (renderer, systems, resources)
- `assets/` — shaders, textures, materials, scenes
- `testbed/` — a minimal sandbox for exercising engine features
- `games/` — real games built on the engine (e.g. `SH`)
- `tools/sdf_editor/` — the visual SDF scene editor

## Building

From the repo root:

```
./build-all.sh    # compiles the engine, testbed, games, and tests
./post-build.sh   # compiles shaders and copies assets/ into bin/
```

(`build-all.bat` / `post-build.bat` on Windows.)

Run any built target from inside `bin/` (working directory matters — asset paths are relative to it):

```
cd bin
./testbed
./SH
```

The SDF editor is a separate CMake project:

```
cd tools/sdf_editor
cmake -B build && cmake --build build
cd ../../bin
../tools/sdf_editor/build/sdf_editor
```
