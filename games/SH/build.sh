#!/bin/bash
# Build script for SH
set echo on

# All paths below are relative to this script's directory, so make the
# script runnable from anywhere (e.g. from bin/).
cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1

mkdir -p ../../bin

# Get a list of all the .cpp files.
cppFilenames=$(find . -type f -name "*.cpp")

assembly="SH"
compilerFlags="-g -fdeclspec -fPIC -std=c++20"
# -Wall -Werror
includeFlags="-Isrc -I../../engine/src/"
linkerFlags="-L../../bin/ -lengine -Wl,-rpath,\$ORIGIN"
defines="-D_DEBUG -DKIMPORT"

echo "Building $assembly..."
echo clang++ $cppFilenames $compilerFlags -o ../../bin/$assembly $defines $includeFlags $linkerFlags
clang++ $cppFilenames $compilerFlags -o ../../bin/$assembly $defines $includeFlags $linkerFlags
