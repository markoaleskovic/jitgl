cmake_minimum_required(VERSION 3.22)
project(jit_ide C CXX)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# ── GLFW ────────────────────────────────────────────────────────────────────
FetchContent_Declare(glfw
    URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
    URL_HASH SHA256=c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01
    EXCLUDE_FROM_ALL
)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# ── GLAD ────────────────────────────────────────────────────────────────────
FetchContent_Declare(glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG        v2.0.6
    SOURCE_SUBDIR  cmake
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(glad)
glad_add_library(glad_gl_core_33 STATIC REPRODUCIBLE LOADER API gl:core=3.3)

# ── ImGui ───────────────────────────────────────────────────────────────────
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.8-docking.tar.gz
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC glfw glad_gl_core_33)

# ── ImGuiColorTextEdit ───────────────────────────────────────────────────────
FetchContent_Declare(text_editor
    URL https://github.com/BalazsJako/ImGuiColorTextEdit/archive/refs/heads/master.tar.gz
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(text_editor)

# ── LLVM + Clang ─────────────────────────────────────────────────────────────
find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")
message(STATUS "Found Clang at ${Clang_DIR}")

# Query Clang resource dir at configure time (provides stddef.h, stdarg.h, etc.)
execute_process(
    COMMAND clang -print-resource-dir
    OUTPUT_VARIABLE CLANG_RESOURCE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
message(STATUS "Clang resource directory: ${CLANG_RESOURCE_DIR}")

# ── OpenGL ───────────────────────────────────────────────────────────────────
find_package(OpenGL REQUIRED)

# ── Workspace setup ──────────────────────────────────────────────────────────
# Create workspace directory in the binary dir (where the app is run from)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/workspace)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/runtime)

# Copy engine.hpp preamble into workspace so JIT can find it at runtime
configure_file(
        ${CMAKE_SOURCE_DIR}/src/runtime/engine.hpp
        ${CMAKE_BINARY_DIR}/runtime/engine.hpp
        COPYONLY
)

# ── jit_ide executable ───────────────────────────────────────────────────────
# ASTDiffer.cpp removed -- no longer needed in the new architecture
add_executable(jit_ide
    src/main.cpp
    src/core/Engine.cpp
    src/ui/EditorUI.cpp
    src/jit/JitEngine.cpp
    src/watcher/FileWatcher.cpp
    ${text_editor_SOURCE_DIR}/TextEditor.cpp
        src/core/WorkspaceManager.cpp
        src/system/ConsoleRedirectSession.cpp
)

target_include_directories(jit_ide PRIVATE
        src
        ${LLVM_INCLUDE_DIRS}
        ${CLANG_INCLUDE_DIRS}
        ${text_editor_SOURCE_DIR}
        $<TARGET_PROPERTY:glad_gl_core_33,INTERFACE_INCLUDE_DIRECTORIES>
)

# Convert LLVM definitions string into a proper CMake list (avoids parsing issues)
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})

target_compile_definitions(jit_ide PRIVATE
    ${LLVM_DEFINITIONS_LIST}
    JIT_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    JIT_PROJECT_BINARY_DIR="${CMAKE_BINARY_DIR}"
    JIT_CLANG_RESOURCE_DIR="${CLANG_RESOURCE_DIR}"
    # Generator expression gives the exact include paths that glad_gl_core_33 was built with.
    # Pipe-separated because a target can have multiple interface include dirs.
    JIT_GLAD_INCLUDE_DIR="$<JOIN:$<TARGET_PROPERTY:glad_gl_core_33,INTERFACE_INCLUDE_DIRECTORIES>,|>"
)

# Match LLVM's RTTI setting -- mixing RTTI/no-RTTI causes vtable link errors
if(NOT LLVM_ENABLE_RTTI)
    target_compile_options(jit_ide PRIVATE -fno-rtti)
endif()

target_link_libraries(jit_ide PRIVATE
    imgui_lib
    glfw
    glad_gl_core_33
    OpenGL::GL
    clang-cpp
    LLVM
)

set_target_properties(jit_ide PROPERTIES ENABLE_EXPORTS TRUE)
# This is required on Linux so the ORC JIT can link user code against the host's GLAD functions
set_target_properties(jit_ide PROPERTIES ENABLE_EXPORTS TRUE)

# libclang provides the clang-c/Index.h C API used by some tools.
# Only needed if you use ASTDiffer. Kept here as a safety net.
if(TARGET libclang)
    target_link_libraries(jit_ide PRIVATE libclang)
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(jit_ide PRIVATE dl pthread)
    target_link_options(jit_ide PRIVATE -rdynamic)
endif()

add_custom_command(TARGET jit_ide POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/assets"
    "${CMAKE_CURRENT_BINARY_DIR}/assets"
)