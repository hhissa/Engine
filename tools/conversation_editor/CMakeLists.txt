cmake_minimum_required(VERSION 3.16)

# Match engine/build.sh and testbed/build.sh's toolchain -- see
# tools/sdf_editor/CMakeLists.txt's identical comment for why (engine/src/
# core/event.h's EventContext union needs clang, not GCC). Must be set
# before project() -- CMake already picks and caches the compiler during
# that call.
set(CMAKE_CXX_COMPILER clang++)

project(conversation_editor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Signals/slots (moc) is the only Qt build magic this project needs -- no
# embedded Vulkan viewport here (unlike sdf_editor), so just Widgets, no
# Gui/QWindow requirement.
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../engine")
set(ENGINE_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../bin")

add_executable(conversation_editor
  src/main.cpp
  src/main_window.h
  src/main_window.cpp
  src/graph_scene.h
  src/graph_scene.cpp
  src/question_node.h
  src/question_node.cpp
  src/connection_item.h
  src/connection_item.cpp
  src/conversation_io.h
  src/conversation_io.cpp
)

target_include_directories(conversation_editor PRIVATE
  src
  "${ENGINE_ROOT}/src"
)

target_compile_definitions(conversation_editor PRIVATE KIMPORT)

# load_conversation_file()'s KERROR/KWARN calls (engine/src/resources/
# conversation.cpp) resolve to Logger::log_output, already compiled into
# libengine.so -- link against that instead of recompiling the engine.
find_library(ENGINE_LIB NAMES engine PATHS "${ENGINE_BIN_DIR}" REQUIRED)
target_link_libraries(conversation_editor PRIVATE Qt6::Widgets "${ENGINE_LIB}")
set_target_properties(conversation_editor PROPERTIES
  BUILD_RPATH "${ENGINE_BIN_DIR}"
)
