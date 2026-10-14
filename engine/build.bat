@ECHO OFF
REM Build script for engine -- the Windows counterpart of build.sh.
REM Usage: build.bat [release]
REM
REM Debug (the default) defines _DEBUG, which turns on Vulkan validation and
REM makes VK_LAYER_KHRONOS_validation REQUIRED (vulkan_backend.cpp fails
REM without it) -- fine on a dev machine with the Vulkan SDK, fatal on a
REM player's. Anything handed to someone else must be a release build.
REM
REM The C runtime is the DLL one in both configs (-fms-runtime-lib): the
REM engine and the game pass std::string/std::vector across engine.dll's
REM boundary, which is only safe when both sides share one heap. The game's
REM build.bat must pick the same config.
SetLocal EnableDelayedExpansion

IF NOT EXIST "..\bin" MKDIR "..\bin"

REM Every .cpp under engine\, via a response file -- the absolute paths FOR /R
REM produces already flirt with cmd.exe's 8191-character command-line limit.
REM platform_linux.cpp is in the list too; it compiles to nothing here.
SET sources=..\bin\engine_sources.rsp
TYPE NUL > "%sources%"
FOR /R %%f IN (*.cpp) DO ECHO "%%f">> "%sources%"

SET assembly=engine
IF /I "%~1"=="release" (
    SET config=release
    SET compilerFlags=-O2 -std=c++20 -fdeclspec -fms-runtime-lib=dll
    SET defines=-DNDEBUG -DKEXPORT -D_CRT_SECURE_NO_WARNINGS
) ELSE (
    SET config=debug
    SET compilerFlags=-g -gcodeview -std=c++20 -fdeclspec -fms-runtime-lib=dll_dbg
    SET defines=-D_DEBUG -DKEXPORT -D_CRT_SECURE_NO_WARNINGS
)
REM -Wall -Werror
SET includeFlags=-Isrc -I"%VULKAN_SDK%\Include"
REM winmm: PlaySound, for audio.cpp.
SET linkerFlags=-shared -luser32 -lwinmm -lvulkan-1 -L"%VULKAN_SDK%\Lib"

ECHO Building %assembly% (%config%)...
REM Also writes ..\bin\engine.lib, the import library the game links against.
clang++ @"%sources%" %compilerFlags% -o ..\bin\%assembly%.dll %defines% %includeFlags% %linkerFlags%
EXIT /B %ERRORLEVEL%
