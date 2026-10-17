cmake_minimum_required(VERSION 3.15)
project(DC2TYPatch)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 20)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Error: Building for x64, but x86 expected. Use x86 Native Tools Command Prompt.")
endif()

# Detours library
set(DETOURS_DIR "${CMAKE_SOURCE_DIR}/Detours/src")
include_directories(${DETOURS_DIR})

add_library(detours STATIC
    ${DETOURS_DIR}/detours.cpp
    ${DETOURS_DIR}/modules.cpp
    ${DETOURS_DIR}/disasm.cpp
    ${DETOURS_DIR}/image.cpp
    ${DETOURS_DIR}/creatwth.cpp
    ${DETOURS_DIR}/disolx86.cpp
)

# ============================================================================
# DC2TYPatch.dll - The actual patch (injected by launcher)
# ============================================================================
add_library(DC2TYPatch SHARED
    src/dc2typatch.cpp
)

target_link_libraries(DC2TYPatch PRIVATE 
    detours
    gdi32
    user32
    advapi32
    comdlg32
)

set_target_properties(DC2TYPatch PROPERTIES
    LINK_FLAGS "/DYNAMICBASE:NO"
    OUTPUT_NAME "DC2TYPatch"
)
target_compile_definitions(DC2TYPatch PRIVATE GAME_DC2TY)

# ============================================================================
# DC2TYLauncher.exe - Starts the game with Japanese locale and injects DC2TYPatch.dll
# ============================================================================
add_executable(DC2TYLauncher WIN32
    src/launcher.cpp
    src/launcher.rc
)

target_include_directories(DC2TYLauncher PRIVATE src)

target_link_libraries(DC2TYLauncher PRIVATE
    user32
    advapi32
)

target_compile_definitions(DC2TYLauncher PRIVATE GAME_DC2TY)

option(DC2TY_BUILD_CHOICE_TESTS "Build the standalone choice typography checks" OFF)
if(DC2TY_BUILD_CHOICE_TESTS)
    enable_testing()
    add_executable(choice_layout_test tests/choice_layout_test.cpp)
    target_include_directories(choice_layout_test PRIVATE src)
    target_link_libraries(choice_layout_test PRIVATE gdi32 user32)
    add_test(NAME choice_layout COMMAND choice_layout_test)
endif()
