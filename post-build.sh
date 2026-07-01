#!/bin/bash

# Run from root directory!
mkdir -p bin/assets
mkdir -p bin/assets/shaders

echo "Compiling shaders..."

GLSLC="$VULKAN_SDK/bin/glslc"
if [ ! -x "$GLSLC" ]; then
  GLSLC="glslc"
fi

echo "assets/shaders/Builtin.ObjectShader.vert.glsl -> bin/assets/shaders/Builtin.ObjectShader.vert.spv"
$GLSLC -fshader-stage=vert assets/shaders/Builtin.ObjectShader.vert.glsl -o bin/assets/shaders/Builtin.ObjectShader.vert.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.ObjectShader.frag.glsl -> bin/assets/shaders/Builtin.ObjectShader.frag.spv"
$GLSLC -fshader-stage=frag assets/shaders/Builtin.ObjectShader.frag.glsl -o bin/assets/shaders/Builtin.ObjectShader.frag.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.RaymarchVoxelize.comp.glsl -> bin/assets/shaders/Builtin.RaymarchVoxelize.comp.spv"
$GLSLC -fshader-stage=compute assets/shaders/Builtin.RaymarchVoxelize.comp.glsl -o bin/assets/shaders/Builtin.RaymarchVoxelize.comp.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.RaymarchShader.comp.glsl -> bin/assets/shaders/Builtin.RaymarchShader.comp.spv"
$GLSLC -fshader-stage=compute assets/shaders/Builtin.RaymarchShader.comp.glsl -o bin/assets/shaders/Builtin.RaymarchShader.comp.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "Copying assets..."
echo cp -R "assets" "bin"
cp -R "assets" "bin"

echo "Done."
