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

echo "assets/shaders/Builtin.ProbeBake.comp.glsl -> bin/assets/shaders/Builtin.ProbeBake.comp.spv"
$GLSLC -fshader-stage=compute assets/shaders/Builtin.ProbeBake.comp.glsl -o bin/assets/shaders/Builtin.ProbeBake.comp.spv
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

echo "assets/shaders/Builtin.BloomBlurH.comp.glsl -> bin/assets/shaders/Builtin.BloomBlurH.comp.spv"
$GLSLC -fshader-stage=compute assets/shaders/Builtin.BloomBlurH.comp.glsl -o bin/assets/shaders/Builtin.BloomBlurH.comp.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.PostComposite.comp.glsl -> bin/assets/shaders/Builtin.PostComposite.comp.spv"
$GLSLC -fshader-stage=compute assets/shaders/Builtin.PostComposite.comp.glsl -o bin/assets/shaders/Builtin.PostComposite.comp.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.UIShader.vert.glsl -> bin/assets/shaders/Builtin.UIShader.vert.spv"
$GLSLC -fshader-stage=vert assets/shaders/Builtin.UIShader.vert.glsl -o bin/assets/shaders/Builtin.UIShader.vert.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.UIShader.frag.glsl -> bin/assets/shaders/Builtin.UIShader.frag.spv"
$GLSLC -fshader-stage=frag assets/shaders/Builtin.UIShader.frag.glsl -o bin/assets/shaders/Builtin.UIShader.frag.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.TextShader.vert.glsl -> bin/assets/shaders/Builtin.TextShader.vert.spv"
$GLSLC -fshader-stage=vert assets/shaders/Builtin.TextShader.vert.glsl -o bin/assets/shaders/Builtin.TextShader.vert.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.TextShader.frag.glsl -> bin/assets/shaders/Builtin.TextShader.frag.spv"
$GLSLC -fshader-stage=frag assets/shaders/Builtin.TextShader.frag.glsl -o bin/assets/shaders/Builtin.TextShader.frag.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.LineShader.vert.glsl -> bin/assets/shaders/Builtin.LineShader.vert.spv"
$GLSLC -fshader-stage=vert assets/shaders/Builtin.LineShader.vert.glsl -o bin/assets/shaders/Builtin.LineShader.vert.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.LineShader.frag.glsl -> bin/assets/shaders/Builtin.LineShader.frag.spv"
$GLSLC -fshader-stage=frag assets/shaders/Builtin.LineShader.frag.glsl -o bin/assets/shaders/Builtin.LineShader.frag.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.SolidQuadShader.vert.glsl -> bin/assets/shaders/Builtin.SolidQuadShader.vert.spv"
$GLSLC -fshader-stage=vert assets/shaders/Builtin.SolidQuadShader.vert.glsl -o bin/assets/shaders/Builtin.SolidQuadShader.vert.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "assets/shaders/Builtin.SolidQuadShader.frag.glsl -> bin/assets/shaders/Builtin.SolidQuadShader.frag.spv"
$GLSLC -fshader-stage=frag assets/shaders/Builtin.SolidQuadShader.frag.glsl -o bin/assets/shaders/Builtin.SolidQuadShader.frag.spv
ERRORLEVEL=$?
if [ $ERRORLEVEL -ne 0 ]
then
echo "Error:"$ERRORLEVEL && exit $ERRORLEVEL
fi

echo "Copying assets..."
echo cp -R "assets" "bin"
cp -R "assets" "bin"

echo "Done."
