@ECHO OFF
REM Build script for SH -- the Windows counterpart of build.sh.
REM Usage: build.bat [release]   (must match the engine's config -- see
REM engine\build.bat for why)
SetLocal EnableDelayedExpansion

REM All paths below are relative to this script's directory, so make the
REM script runnable from anywhere (e.g. from bin\).
PUSHD "%~dp0"

IF NOT EXIST "..\..\bin" MKDIR "..\..\bin"

SET sources=..\..\bin\SH_sources.rsp
TYPE NUL > "%sources%"
FOR /R %%f IN (*.cpp) DO ECHO "%%f">> "%sources%"

SET assembly=SH
IF /I "%~1"=="release" (
    SET config=release
    SET compilerFlags=-O2 -std=c++20 -fdeclspec -fms-runtime-lib=dll
    SET defines=-DNDEBUG -DKIMPORT -D_CRT_SECURE_NO_WARNINGS
) ELSE (
    SET config=debug
    SET compilerFlags=-g -gcodeview -std=c++20 -fdeclspec -fms-runtime-lib=dll_dbg
    SET defines=-D_DEBUG -DKIMPORT -D_CRT_SECURE_NO_WARNINGS
)
REM -Wall -Werror
REM The Vulkan SDK include dir is only here for glm, which it ships.
SET includeFlags=-Isrc -I..\..\engine\src -I"%VULKAN_SDK%\Include"
REM engine.lib is engine.dll's import library; SH.exe finds engine.dll at
REM run time because both live in bin\ (Windows searches the exe's own
REM directory first -- the equivalent of the Linux build's $ORIGIN rpath).
SET linkerFlags=-L..\..\bin -lengine

ECHO Building %assembly% (%config%)...
clang++ @"%sources%" %compilerFlags% -o ..\..\bin\%assembly%.exe %defines% %includeFlags% %linkerFlags%
SET result=%ERRORLEVEL%
POPD
EXIT /B %result%
