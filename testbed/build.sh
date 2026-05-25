#!/bin/bash
# Build script for testbed
set echo on

mkdir -p ../bin

# Get a list of all the .cpp files.
cppFilenames=$(find . -type f -name "*.cpp")

# echo "Files:" $cppFilenames

assembly="testbed"
compilerFlags="-g -fdeclspec -fPIC -std=c++17"
# -fms-extensions
# -Wall -Werror

includeFlags="-Isrc -I../engine/src/"
linkerFlags="-L../bin/ -lengine -Wl,-rpath,."
defines="-D_DEBUG -DKIMPORT"

echo "Building $assembly..."
echo clang++ $cppFilenames $compilerFlags -o ../bin/$assembly $defines $includeFlags $linkerFlags
clang++ $cppFilenames $compilerFlags -o ../bin/$assembly $defines $includeFlags $linkerFlagslang $cFilenames $compilerFlags -o ../bin/$assembly $defines $includeFlags $linkerFlags
