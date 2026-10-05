#!/bin/bash
# Build script for tests
set echo on

mkdir -p ../bin

# Get a list of all the .cpp files.
cppFilenames=$(find . -type f -name "*.cpp")
# Reused directly rather than duplicated -- the same read/write helpers the
# sdf_editor links against (see tools/sdf_editor/CMakeLists.txt).
cppFilenames="$cppFilenames ../testbed/src/sdf_authoring.cpp"

assembly="tests"
compilerFlags="-g -fdeclspec -fPIC -std=c++20"
# -Wall -Werror
includeFlags="-Isrc -I../engine/src/ -I../testbed/src/"
linkerFlags="-L../bin/ -lengine -Wl,-rpath,\$ORIGIN"
defines="-D_DEBUG -DKIMPORT"

echo "Building $assembly..."
clang++ $cppFilenames $compilerFlags -o ../bin/$assembly $defines $includeFlags $linkerFlags
