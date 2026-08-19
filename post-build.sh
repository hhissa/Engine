#!/bin/bash

# Run from root directory!
mkdir -p bin/assets
mkdir -p bin/assets/shaders

echo "Compiling shaders..."

GLSLC="$VULKAN_SDK/bin/glslc"
if [ ! -x "$GLSLC" ]; then
  GLSLC="glslc"
fi

# Compile against the same Vulkan version the engine actually requests
# (VK_API_VERSION_1_2 -- see vulkan_backend.cpp). Without this glslc defaults
# to vulkan1.0, which emits SPIR-V 1.0 modules that declare capabilities like
# descriptor indexing (Builtin.DeferredShade.comp.glsl's nonuniformEXT) as an
# OpExtension instead of core -- valid only if the matching device EXTENSION
# is also enabled, which it isn't, because on a 1.2 device the feature is
# core. Targeting 1.2 emits SPIR-V 1.5 where the capability needs no
# extension declaration at all.
GLSLC_ARGS="--target-env=vulkan1.2"

echo "assets/shaders/Builtin.ObjectShader.vert.glsl -> bin/assets/shaders/Builtin.ObjectShader.vert.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=vert assets/shaders/Builtin.ObjectShader.vert.glsl -o bin/assets/shaders/Builtin.ObjectShader.vert.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.ObjectShader.frag.glsl -> bin/assets/shaders/Builtin.ObjectShader.frag.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=frag assets/shaders/Builtin.ObjectShader.frag.glsl -o bin/assets/shaders/Builtin.ObjectShader.frag.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.RaymarchVoxelize.comp.glsl -> bin/assets/shaders/Builtin.RaymarchVoxelize.comp.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=compute assets/shaders/Builtin.RaymarchVoxelize.comp.glsl -o bin/assets/shaders/Builtin.RaymarchVoxelize.comp.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.ChunkVoxelize.comp.glsl -> bin/assets/shaders/Builtin.ChunkVoxelize.comp.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=compute assets/shaders/Builtin.ChunkVoxelize.comp.glsl -o bin/assets/shaders/Builtin.ChunkVoxelize.comp.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.ChunkedFieldDebugQuery.comp.glsl -> bin/assets/shaders/Builtin.ChunkedFieldDebugQuery.comp.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=compute assets/shaders/Builtin.ChunkedFieldDebugQuery.comp.glsl -o bin/assets/shaders/Builtin.ChunkedFieldDebugQuery.comp.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.ChunkEvict.comp.glsl -> bin/assets/shaders/Builtin.ChunkEvict.comp.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=compute assets/shaders/Builtin.ChunkEvict.comp.glsl -o bin/assets/shaders/Builtin.ChunkEvict.comp.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.ChunkClusterCull.comp.glsl -> bin/assets/shaders/Builtin.ChunkClusterCull.comp.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=compute assets/shaders/Builtin.ChunkClusterCull.comp.glsl -o bin/assets/shaders/Builtin.ChunkClusterCull.comp.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.ChunkShadowSplat.comp.glsl -> bin/assets/shaders/Builtin.ChunkShadowSplat.comp.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=compute assets/shaders/Builtin.ChunkShadowSplat.comp.glsl -o bin/assets/shaders/Builtin.ChunkShadowSplat.comp.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.ChunkPointSplat.comp.glsl -> bin/assets/shaders/Builtin.ChunkPointSplat.comp.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=compute assets/shaders/Builtin.ChunkPointSplat.comp.glsl -o bin/assets/shaders/Builtin.ChunkPointSplat.comp.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.ChunkProbeBake.comp.glsl -> bin/assets/shaders/Builtin.ChunkProbeBake.comp.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=compute assets/shaders/Builtin.ChunkProbeBake.comp.glsl -o bin/assets/shaders/Builtin.ChunkProbeBake.comp.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.ProbeBake.comp.glsl -> bin/assets/shaders/Builtin.ProbeBake.comp.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=compute assets/shaders/Builtin.ProbeBake.comp.glsl -o bin/assets/shaders/Builtin.ProbeBake.comp.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.RaymarchShader.comp.glsl -> bin/assets/shaders/Builtin.RaymarchShader.comp.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=compute assets/shaders/Builtin.RaymarchShader.comp.glsl -o bin/assets/shaders/Builtin.RaymarchShader.comp.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.BloomBlurH.comp.glsl -> bin/assets/shaders/Builtin.BloomBlurH.comp.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=compute assets/shaders/Builtin.BloomBlurH.comp.glsl -o bin/assets/shaders/Builtin.BloomBlurH.comp.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.ChunkVoxelCascade.comp.glsl -> bin/assets/shaders/Builtin.ChunkVoxelCascade.comp.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=compute assets/shaders/Builtin.ChunkVoxelCascade.comp.glsl -o bin/assets/shaders/Builtin.ChunkVoxelCascade.comp.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.StochasticAo.comp.glsl -> bin/assets/shaders/Builtin.StochasticAo.comp.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=compute assets/shaders/Builtin.StochasticAo.comp.glsl -o bin/assets/shaders/Builtin.StochasticAo.comp.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.DeferredShade.comp.glsl -> bin/assets/shaders/Builtin.DeferredShade.comp.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=compute assets/shaders/Builtin.DeferredShade.comp.glsl -o bin/assets/shaders/Builtin.DeferredShade.comp.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.TaaResolve.comp.glsl -> bin/assets/shaders/Builtin.TaaResolve.comp.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=compute assets/shaders/Builtin.TaaResolve.comp.glsl -o bin/assets/shaders/Builtin.TaaResolve.comp.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.PostComposite.comp.glsl -> bin/assets/shaders/Builtin.PostComposite.comp.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=compute assets/shaders/Builtin.PostComposite.comp.glsl -o bin/assets/shaders/Builtin.PostComposite.comp.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.UIShader.vert.glsl -> bin/assets/shaders/Builtin.UIShader.vert.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=vert assets/shaders/Builtin.UIShader.vert.glsl -o bin/assets/shaders/Builtin.UIShader.vert.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.UIShader.frag.glsl -> bin/assets/shaders/Builtin.UIShader.frag.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=frag assets/shaders/Builtin.UIShader.frag.glsl -o bin/assets/shaders/Builtin.UIShader.frag.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.TextShader.vert.glsl -> bin/assets/shaders/Builtin.TextShader.vert.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=vert assets/shaders/Builtin.TextShader.vert.glsl -o bin/assets/shaders/Builtin.TextShader.vert.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.TextShader.frag.glsl -> bin/assets/shaders/Builtin.TextShader.frag.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=frag assets/shaders/Builtin.TextShader.frag.glsl -o bin/assets/shaders/Builtin.TextShader.frag.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.LineShader.vert.glsl -> bin/assets/shaders/Builtin.LineShader.vert.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=vert assets/shaders/Builtin.LineShader.vert.glsl -o bin/assets/shaders/Builtin.LineShader.vert.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.LineShader.frag.glsl -> bin/assets/shaders/Builtin.LineShader.frag.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=frag assets/shaders/Builtin.LineShader.frag.glsl -o bin/assets/shaders/Builtin.LineShader.frag.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.SolidQuadShader.vert.glsl -> bin/assets/shaders/Builtin.SolidQuadShader.vert.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=vert assets/shaders/Builtin.SolidQuadShader.vert.glsl -o bin/assets/shaders/Builtin.SolidQuadShader.vert.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.SolidQuadShader.frag.glsl -> bin/assets/shaders/Builtin.SolidQuadShader.frag.spv"
$GLSLC $GLSLC_ARGS -fshader-stage=frag assets/shaders/Builtin.SolidQuadShader.frag.glsl -o bin/assets/shaders/Builtin.SolidQuadShader.frag.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

# --- Compiled-shader sanity checks -------------------------------------
# A shader's workgroup shape and its push-constant block are contracts with
# the engine that nothing on either side validates at runtime: get them wrong
# and the dispatch silently covers the wrong cells, or every push constant
# after the missing field shifts by four bytes. Both have happened here, and
# both presented as "geometry is missing" with no error anywhere.
#
# Checking the COMPILED module rather than the source is deliberate -- what
# matters is what the GPU ends up running.
if command -v spirv-dis >/dev/null 2>&1; then
  echo "Verifying compiled shader contracts..."
  CHUNK_VOX_SPV="bin/assets/shaders/Builtin.ChunkVoxelize.comp.spv"
  # Must match voxelize_chunk_batch()'s dispatch math (local_size_xy = 8, one
  # cell row per invocation in Z).
  if ! spirv-dis "$CHUNK_VOX_SPV" | grep -q "LocalSize 8 8 1"; then
    echo "ERROR: $CHUNK_VOX_SPV workgroup size is not 8 8 1 -- the engine's"
    echo "       dispatch assumes it is. See the shader's own comment."
    exit 1
  fi
  echo "  Builtin.ChunkVoxelize.comp.spv: LocalSize 8 8 1 OK"
fi

echo "Copying assets..."
echo cp -R "assets" "bin"
cp -R "assets" "bin"

echo "Done."
