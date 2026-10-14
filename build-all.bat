@ECHO OFF
REM Build everything that runs on Windows. Usage: build-all.bat [release]
REM
REM testbed and tests are Linux-only for now: they call engine internals
REM that aren't KAPI-exported (material_def_*, ChunkStreamingManager, ...).
REM A Linux .so exports everything regardless; a Windows DLL exports only
REM what's marked, so those would fail to link here.

ECHO Building everything...

PUSHD engine
CALL build.bat %1 || (POPD & ECHO Error building engine. & EXIT /B 1)
POPD

PUSHD games\SH
CALL build.bat %1 || (POPD & ECHO Error building SH. & EXIT /B 1)
POPD

ECHO All assemblies built successfully.
