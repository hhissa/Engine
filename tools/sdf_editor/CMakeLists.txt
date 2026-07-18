cmake_minimum_required(VERSION 3.16)

# Match engine/build.sh and testbed/build.sh's toolchain: engine/src/core/
# event.h's EventContext union has members named identically to the
# typedefs they're typed as (e.g. `u64 u64[2];`), which clang accepts but
# GCC rejects under -Wchanges-meaning (an error, not just a warning, at
# GCC's default diagnostic level). Must be set before project() -- CMake
# already picks and caches the compiler during that call.
set(CMAKE_CXX_COMPILER clang++)

project(sdf_editor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt widgets use signals/slots (moc) and, in a couple of spots, .ui-less
# manual layout only -- AUTOMOC is the only Qt build magic this project
# actually needs.
set(CMAKE_AUTOMOC ON)

# Gui is needed for QWindow/QNativeInterface::QX11Application/QSurface --
# see scene_viewport.cpp, which embeds the engine's Vulkan rendering
# directly into a QWindow rather than going through QWidget alone.
find_package(Qt6 REQUIRED COMPONENTS Widgets Gui)

set(ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../engine")
set(TESTBED_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../testbed")
set(ENGINE_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../bin")

add_executable(sdf_editor
  src/main.cpp
  src/main_window.cpp
  src/main_window.h
  src/scene_viewport.cpp
  src/scene_viewport.h
  src/ray_intersect.h
  # Reused directly rather than duplicated -- the same read/write/builder
  # functions testbed uses (see testbed/src/sdf_authoring.h/.cpp).
  "${TESTBED_ROOT}/src/sdf_authoring.cpp"
)

target_include_directories(sdf_editor PRIVATE
  src
  "${ENGINE_ROOT}/src"
  "${TESTBED_ROOT}/src"
)

# Not functionally load-bearing on Linux (defines.h's KAPI only branches on
# KEXPORT, not KIMPORT) -- passed anyway to match testbed/build.sh's
# convention for anything consuming engine headers as a client rather than
# building the engine itself.
target_compile_definitions(sdf_editor PRIVATE KIMPORT)

# sdf_authoring.cpp/sdf_scene.cpp's KERROR/KWARN calls resolve to
# Logger::log_output, defined in logger.cpp -- already compiled into
# libengine.so (engine/build.sh globs every engine/src/**/*.cpp), so link
# against that instead of recompiling the whole engine into this tool.
find_library(ENGINE_LIB NAMES engine PATHS "${ENGINE_BIN_DIR}" REQUIRED)
target_link_libraries(sdf_editor PRIVATE Qt6::Widgets Qt6::Gui "${ENGINE_LIB}")
set_target_properties(sdf_editor PROPERTIES
  BUILD_RPATH "${ENGINE_BIN_DIR}"
)
