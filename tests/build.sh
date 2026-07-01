#!/bin/bash
# Build script for tests
set echo on

mkdir -p ../bin

# Get a list of all the .cpp files.
cppFilenames=$(find . -type f -name "*.cpp")

assembly="tests"
compilerFlags="-g -fdeclspec -fPIC -std=c++20"
# -Wall -Werror
includeFlags="-Isrc -I../engine/src/"
linkerFlags="-L../bin/ -lengine -Wl,-rpath,\$ORIGIN"
defines="-D_DEBUG -DKIMPORT"

echo "Building $assembly..."
clang++ $cppFilenames $compilerFlags -o ../bin/$assembly $defines $includeFlags $linkerFlags
