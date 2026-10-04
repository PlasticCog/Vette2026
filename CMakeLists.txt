cmake_minimum_required(VERSION 3.24)
project(vette2026 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# SDL3: an installed package if there is one, otherwise built from source.
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
    include(FetchContent)
    # Static SDL plus the static MSVC runtime, so the exe runs standalone.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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
    src/platform/framebuffer.cpp
    src/platform/presenter.cpp
)
target_include_directories(vette2026 PRIVATE src)

if(TARGET SDL3::SDL3-static)
    target_link_libraries(vette2026 PRIVATE SDL3::SDL3-static)
else()
    target_link_libraries(vette2026 PRIVATE SDL3::SDL3)
endif()

if(MSVC)
    target_compile_options(vette2026 PRIVATE /W4 /utf-8)
else()
    target_compile_options(vette2026 PRIVATE -Wall -Wextra -Wpedantic)
endif()
