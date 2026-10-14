@ECHO OFF
REM The Windows counterpart of post-build.sh -- see it for why the glslc
REM flags are what they are. Run from root directory!
SetLocal

IF NOT EXIST "bin\assets\shaders\" MKDIR "bin\assets\shaders"

ECHO Compiling shaders...

SET GLSLC=%VULKAN_SDK%\Bin\glslc.exe
IF NOT EXIST "%GLSLC%" SET GLSLC=glslc
SET GLSLC_ARGS=--target-env=vulkan1.2 -O

REM Every *.vert/*.frag/*.comp.glsl (the *.inc.glsl files are #include-only),
REM rather than a hand-kept list that drifts out of date: %%~nf drops the
REM trailing .glsl, so Builtin.X.comp.glsl -> Builtin.X.comp.spv.
FOR %%s IN (vert frag comp) DO (
    FOR %%f IN (assets\shaders\*.%%s.glsl) DO (
        ECHO %%f -^> bin\assets\shaders\%%~nf.spv
        "%GLSLC%" %GLSLC_ARGS% -fshader-stage=%%s %%f -o bin\assets\shaders\%%~nf.spv || (ECHO Error compiling %%f & EXIT /B 1)
    )
)

REM Compiled-shader sanity checks -- see post-build.sh. Optional, like there:
REM skipped when spirv-dis (part of the Vulkan SDK) isn't on PATH.
WHERE spirv-dis >NUL 2>&1
IF %ERRORLEVEL% EQU 0 (
    ECHO Verifying compiled shader contracts...
    spirv-dis bin\assets\shaders\Builtin.ChunkVoxelize.comp.spv | FINDSTR /C:"LocalSize 8 8 1" >NUL || (
        ECHO ERROR: bin\assets\shaders\Builtin.ChunkVoxelize.comp.spv workgroup size is not 8 8 1 -- the engine's
        ECHO        dispatch assumes it is. See the shader's own comment.
        EXIT /B 1
    )
    ECHO   Builtin.ChunkVoxelize.comp.spv: LocalSize 8 8 1 OK
)

ECHO Copying assets...
XCOPY "assets" "bin\assets" /h /i /c /k /e /r /y >NUL || EXIT /B 1

ECHO Done.
