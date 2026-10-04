cmake_minimum_required(VERSION 3.24)
project(vette2026 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VETTE_BUILD_GAME "Build the game executable (needs SDL3)" ON)
option(VETTE_BUILD_TESTS "Build the unit tests" ON)

if(VETTE_BUILD_GAME)
    find_package(SDL3 CONFIG QUIET)
endif()
if(NOT SDL3_FOUND)
    # Static MSVC runtime for every target, so the exe runs standalone. Only when SDL is built from
    # source: a prebuilt SDL3 package is usually built against the DLL runtime.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

function(vette_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# --- Emulation host: runs the original VETTE.EXE (Phase 1), no SDL dependency ---------------------
file(GLOB VETTE_HOST_SOURCES CONFIGURE_DEPENDS src/host/*.cpp)
add_library(vette_host STATIC ${VETTE_HOST_SOURCES})
target_include_directories(vette_host PUBLIC src)
vette_warnings(vette_host)

# Headless runner: boots the hosted game, injects keys, saves screenshots (no SDL needed).
add_executable(vette_run src/tools/vette_run.cpp)
target_link_libraries(vette_run PRIVATE vette_host)
vette_warnings(vette_run)

# --- Game executable -----------------------------------------------------------------------------
if(VETTE_BUILD_GAME)
    # SDL3: the installed package found above, otherwise built from source (static).
    if(NOT SDL3_FOUND)
        include(FetchContent)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(SDL3
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG release-3.4.18
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(SDL3)
    endif()

    add_executable(vette2026
        src/main.cpp
        src/assets/planar.cpp
        src/assets/rle.cpp
        src/core/crc32.cpp
        src/core/game_dir.cpp
        src/platform/audio.cpp
        src/platform/framebuffer.cpp
        src/platform/keymap.cpp
        src/platform/presenter.cpp
    )
    target_include_directories(vette2026 PRIVATE src)
    target_link_libraries(vette2026 PRIVATE vette_host)
    if(TARGET SDL3::SDL3-static)
        target_link_libraries(vette2026 PRIVATE SDL3::SDL3-static)
    else()
        target_link_libraries(vette2026 PRIVATE SDL3::SDL3)
    endif()
    vette_warnings(vette2026)
endif()

# --- Tests ---------------------------------------------------------------------------------------
# vette_tests runs everything. Per-component targets (vette_cpu_tests, vette_ega_tests) compile only
# their own sources, so components under parallel development don't break each other's builds.
if(VETTE_BUILD_TESTS)
    file(GLOB VETTE_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
    add_executable(vette_tests ${VETTE_TEST_SOURCES})
    target_link_libraries(vette_tests PRIVATE vette_host)
    target_include_directories(vette_tests PRIVATE tests)
    vette_warnings(vette_tests)

    function(vette_component_tests name)
        foreach(src IN LISTS ARGN)
            if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${src})
                return()  # component not written yet; re-run CMake once it exists
            endif()
        endforeach()
        file(GLOB tests CONFIGURE_DEPENDS tests/${name}_*.cpp)
        add_executable(vette_${name}_tests tests/test_main.cpp ${tests} ${ARGN})
        target_include_directories(vette_${name}_tests PRIVATE src tests)
        vette_warnings(vette_${name}_tests)
    endfunction()
    vette_component_tests(cpu src/host/cpu.cpp)
    vette_component_tests(ega src/host/ega.cpp)
    vette_component_tests(devices src/host/pic.cpp src/host/pit.cpp src/host/speaker.cpp)
endif()
