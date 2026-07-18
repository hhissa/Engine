REM Build script for SH
@ECHO OFF
SetLocal EnableDelayedExpansion

REM Get a list of all the .cpp files.
SET cppFilenames=
FOR /R %%f in (*.cpp) do (
    SET cppFilenames=!cppFilenames! %%f
)

SET assembly=SH
SET compilerFlags=-g
REM -Wall -Werror
SET includeFlags=-Isrc -I../../engine/src/
SET linkerFlags=-L../../bin/ -lengine.lib
SET defines=-D_DEBUG -DKIMPORT

ECHO "Building %assembly%%..."
clang++ %cppFilenames% %compilerFlags% -o ../../bin/%assembly%.exe %defines% %includeFlags% %linkerFlags%
