cmake_minimum_required(VERSION 3.22)
project(jit_ide C CXX)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

FetchContent_Declare(glfw
    URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
    URL_HASH SHA256=c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01
    EXCLUDE_FROM_ALL
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG        v2.0.6
    SOURCE_SUBDIR  cmake
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(glad)

glad_add_library(glad_gl_core_33 STATIC REPRODUCIBLE LOADER API gl:core=3.3)

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

FetchContent_Declare(text_editor
    URL https://github.com/BalazsJako/ImGuiColorTextEdit/archive/refs/heads/master.tar.gz
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(text_editor)

target_link_libraries(imgui_lib PUBLIC 
    glfw 
    glad_gl_core_33
)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

set(CLANG_LIBS
    clangInterpreter
    clangFrontend
    clangDriver
    clangSerialization
    clangCodeGen
    clangParse
    clangSema
    clangAnalysis
    clangAST
    clangBasic
    clangEdit
    clangLex
)
file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/src)

add_executable(jit_ide
    src/main.cpp
    src/core/Engine.cpp
    src/ui/EditorUI.cpp
    ${text_editor_SOURCE_DIR}/TextEditor.cpp
)

target_include_directories(jit_ide PRIVATE
    src
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
    ${text_editor_SOURCE_DIR}
)

target_compile_definitions(jit_ide PRIVATE ${LLVM_DEFINITIONS})

find_package(OpenGL REQUIRED)

target_link_libraries(jit_ide PRIVATE
    imgui_lib
    glfw
    glad_gl_core_33
    OpenGL::GL
    
    clang-cpp 
    
    LLVM
)

if(UNIX AND NOT APPLE)
    target_link_libraries(jit_ide PRIVATE dl pthread)
endif()