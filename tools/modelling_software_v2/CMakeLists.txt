cmake_minimum_required(VERSION 3.16)

# Match engine/build.sh and testbed/build.sh's toolchain: engine/src/core/
# event.h's EventContext union has members named identically to the
# typedefs they're typed as (e.g. `u64 u64[2];`), which clang accepts but
# GCC rejects under -Wchanges-meaning (an error, not just a warning, at
# GCC's default diagnostic level). Must be set before project() -- CMake
# already picks and caches the compiler during that call.
set(CMAKE_CXX_COMPILER clang++)

project(modelling_software_v2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt widgets use signals/slots (moc) -- AUTOMOC is the only Qt build magic
# this project actually needs.
set(CMAKE_AUTOMOC ON)

# Gui is needed for QWindow/QNativeInterface::QX11Application/QSurface --
# see scene_viewport.cpp, which embeds the engine's Vulkan rendering
# directly into a QWindow rather than going through QWidget alone.
find_package(Qt6 REQUIRED COMPONENTS Widgets Gui)

set(ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../engine")
set(ENGINE_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../bin")

# Unlike tools/sdf_editor (deleted in M0 along with the CSG SdfScene it
# authored), this tool has no CSG scene file reader/builder to link --
# the new sculpt brush path is driven entirely through
# renderer_sculpt_stamp_shape() (engine/src/renderer/renderer_frontend.h),
# so there is no testbed/src/sdf_authoring.cpp-equivalent dependency here.
add_executable(modelling_software_v2
  src/main.cpp
  src/controller_mode.cpp
  src/controller_mode.h
  src/sculpt_shared.h
  src/main_window.cpp
  src/main_window.h
  src/scene_viewport.cpp
  src/scene_viewport.h
  src/sculpt_document.h
)

target_include_directories(modelling_software_v2 PRIVATE
  src
  "${ENGINE_ROOT}/src"
)

# Not functionally load-bearing on Linux (defines.h's KAPI only branches on
# KEXPORT, not KIMPORT) -- passed anyway to match testbed/build.sh's
# convention for anything consuming engine headers as a client rather than
# building the engine itself.
target_compile_definitions(modelling_software_v2 PRIVATE KIMPORT)

# Link against the already-built libengine.so (engine/build.sh globs every
# engine/src/**/*.cpp) rather than recompiling the whole engine into this
# tool.
find_library(ENGINE_LIB NAMES engine PATHS "${ENGINE_BIN_DIR}" REQUIRED)
target_link_libraries(modelling_software_v2 PRIVATE Qt6::Widgets Qt6::Gui "${ENGINE_LIB}")
set_target_properties(modelling_software_v2 PROPERTIES
  BUILD_RPATH "${ENGINE_BIN_DIR}"
)
