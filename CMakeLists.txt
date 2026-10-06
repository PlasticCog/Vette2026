cmake_minimum_required(VERSION 3.24)
project(vette2026 VERSION 0.1.6 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VETTE_BUILD_GAME "Build the game executable (needs SDL3)" ON)
option(VETTE_BUILD_TESTS "Build the unit tests" ON)

include(FetchContent)

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

# Adds a tool when its source file exists (tools under parallel development appear as they're written).
function(vette_tool name)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/tools/${name}.cpp)
        add_executable(${name} src/tools/${name}.cpp)
        target_link_libraries(${name} PRIVATE ${ARGN})
        vette_warnings(${name})
    endif()
endfunction()

# --- Emulation host: runs the original VETTE.EXE (Phase 1), no SDL dependency ---------------------
file(GLOB VETTE_HOST_SOURCES CONFIGURE_DEPENDS src/host/*.cpp)
add_library(vette_host STATIC ${VETTE_HOST_SOURCES})
target_include_directories(vette_host PUBLIC src)
vette_warnings(vette_host)

# --- Native ports of original functions (Phase 2), verified against the hosted original -------------
file(GLOB VETTE_GAME_SOURCES CONFIGURE_DEPENDS src/game/*.cpp)
add_library(vette_game STATIC ${VETTE_GAME_SOURCES})
target_link_libraries(vette_game PUBLIC vette_host)
vette_warnings(vette_game)

# --- Core: game folders, settings, and the original versions' file formats (no SDL) -----------------
file(GLOB VETTE_CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp src/assets/*.cpp)
list(REMOVE_ITEM VETTE_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/core/game_dir.cpp)  # SDL: the game's
add_library(vette_core STATIC ${VETTE_CORE_SOURCES})
target_include_directories(vette_core PUBLIC src)
vette_warnings(vette_core)

# --- Enhanced layer (src/enhanced: the city extracted from the running game, and the renderer that
# --- draws it at any resolution and draw distance) ----------------------------------------------------
file(GLOB VETTE_ENHANCED_SOURCES CONFIGURE_DEPENDS src/enhanced/*.cpp)
add_library(vette_enhanced STATIC ${VETTE_ENHANCED_SOURCES})
target_link_libraries(vette_enhanced PUBLIC vette_game)
vette_warnings(vette_enhanced)

# --- Sound: FM chips (ymfm), digitized samples, the mixer and the enhanced sound sources ------------
# ymfm (BSD-3-Clause, Aaron Giles) emulates the AdLib's YM3812 and the PC-98's YM2203.
FetchContent_Declare(ymfm
    GIT_REPOSITORY https://github.com/aaronsgiles/ymfm.git
    GIT_TAG 81aec25ccbb98f4873a255f7551ac4dadac59b4a)
FetchContent_MakeAvailable(ymfm)
file(GLOB YMFM_SOURCES ${ymfm_SOURCE_DIR}/src/*.cpp)
add_library(ymfm STATIC ${YMFM_SOURCES})
target_include_directories(ymfm SYSTEM PUBLIC ${ymfm_SOURCE_DIR}/src)
set_target_properties(ymfm PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(ymfm PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>)  # third-party

file(GLOB VETTE_SOUND_SOURCES CONFIGURE_DEPENDS src/sound/*.cpp)
add_library(vette_sound STATIC ${VETTE_SOUND_SOURCES})
target_link_libraries(vette_sound PUBLIC vette_game vette_core ymfm)
vette_warnings(vette_sound)

# --- Graphics: the PC-98 and Mac versions' art in place of the DOS screens (once it exists) ----------
file(GLOB VETTE_GRAPHICS_SOURCES CONFIGURE_DEPENDS src/graphics/*.cpp)
if(VETTE_GRAPHICS_SOURCES)
    add_library(vette_graphics STATIC ${VETTE_GRAPHICS_SOURCES})
    target_link_libraries(vette_graphics PUBLIC vette_game vette_core)
    vette_warnings(vette_graphics)
endif()

# Headless runner: boots the hosted game, injects keys, saves screenshots (no SDL needed).
vette_tool(vette_run vette_game vette_core)
# Differential fuzzer: calls each native port with random inputs under Verify mode.
vette_tool(vette_fuzz vette_game)
# World extraction tool: dumps the Enhanced world model and validates it against the original.
vette_tool(vette_world vette_enhanced vette_core)
# The other versions' files: Mac resources and sounds, PC-98 disk files and FM sound, art.
vette_tool(vette_mac vette_sound)
vette_tool(vette_pc98 vette_sound)
if(TARGET vette_graphics)
    vette_tool(vette_gfx vette_graphics)
endif()

# --- Game executable -----------------------------------------------------------------------------
if(VETTE_BUILD_GAME)
    # SDL3: the installed package found above, otherwise built from source (static).
    if(NOT SDL3_FOUND)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(SDL3
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG release-3.4.18
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(SDL3)
    endif()
    if(TARGET SDL3::SDL3-static)
        set(VETTE_SDL SDL3::SDL3-static)
    else()
        set(VETTE_SDL SDL3::SDL3)
    endif()

    add_executable(vette2026
        src/main.cpp
        src/core/game_dir.cpp
        src/platform/audio.cpp
        src/platform/framebuffer.cpp
        src/platform/gamepad.cpp
        src/platform/keymap.cpp
        src/platform/presenter.cpp
        src/ui/app_icon.cpp
        src/ui/canvas.cpp
        src/ui/launcher.cpp
    )
    # The program icon is written at build time from src/ui/app_icon.cpp, so there's one copy of the
    # picture: Windows compiles it into the exe's resources (Explorer, taskbar, and SDL uses it for the
    # window); macOS puts it in the app bundle.
    if((WIN32 OR APPLE) AND NOT CMAKE_CROSSCOMPILING)
        add_executable(vette_icon src/tools/vette_icon.cpp src/ui/app_icon.cpp)
        target_include_directories(vette_icon PRIVATE src)
        target_link_libraries(vette_icon PRIVATE vette_core)
        vette_warnings(vette_icon)
    endif()
    if(APPLE AND NOT CMAKE_CROSSCOMPILING)
        set(VETTE_ICNS ${CMAKE_CURRENT_BINARY_DIR}/vette2026.icns)
        add_custom_command(OUTPUT ${VETTE_ICNS} COMMAND vette_icon ${VETTE_ICNS} DEPENDS vette_icon VERBATIM
                           COMMENT "Writing vette2026.icns")
        target_sources(vette2026 PRIVATE ${VETTE_ICNS})
        set_source_files_properties(${VETTE_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        set_target_properties(vette2026 PROPERTIES
            MACOSX_BUNDLE TRUE
            OUTPUT_NAME "VETTE! 2026"
            MACOSX_BUNDLE_BUNDLE_NAME "VETTE! 2026"
            MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.plasticcog.vette2026"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_ICON_FILE vette2026.icns)
    endif()
    if(WIN32 AND NOT CMAKE_CROSSCOMPILING)
        set(VETTE_ICO ${CMAKE_CURRENT_BINARY_DIR}/vette2026.ico)
        set(VETTE_RC ${CMAKE_CURRENT_BINARY_DIR}/vette2026.rc)
        add_custom_command(OUTPUT ${VETTE_ICO} COMMAND vette_icon ${VETTE_ICO} DEPENDS vette_icon VERBATIM
                           COMMENT "Writing vette2026.ico")
        file(CONFIGURE OUTPUT ${VETTE_RC} CONTENT "1 ICON \"@VETTE_ICO@\"\n" @ONLY)
        set_source_files_properties(${VETTE_RC} PROPERTIES OBJECT_DEPENDS ${VETTE_ICO})
        target_sources(vette2026 PRIVATE ${VETTE_RC} ${VETTE_ICO})
    endif()
    target_compile_definitions(vette2026 PRIVATE VETTE_VERSION="${PROJECT_VERSION}")
    target_include_directories(vette2026 PRIVATE src)
    target_link_libraries(vette2026 PRIVATE vette_enhanced vette_sound vette_core ${VETTE_SDL})
    if(TARGET vette_graphics)
        target_link_libraries(vette2026 PRIVATE vette_graphics)
    endif()
    vette_warnings(vette2026)

    # The sound editor (a separate program): edits the AdLib sound bank the game plays.
    add_executable(vette_sfx
        src/tools/vette_sfx.cpp
        src/core/game_dir.cpp
        src/platform/audio.cpp
        src/platform/framebuffer.cpp
        src/platform/presenter.cpp
        src/ui/app_icon.cpp
        src/ui/canvas.cpp
    )
    target_include_directories(vette_sfx PRIVATE src)
    target_link_libraries(vette_sfx PRIVATE vette_enhanced vette_sound vette_core ${VETTE_SDL})
    if(WIN32 AND NOT CMAKE_CROSSCOMPILING)
        target_sources(vette_sfx PRIVATE ${VETTE_RC} ${VETTE_ICO})
        set_target_properties(vette_sfx PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
    if(APPLE AND NOT CMAKE_CROSSCOMPILING)
        target_sources(vette_sfx PRIVATE ${VETTE_ICNS})
        set_target_properties(vette_sfx PROPERTIES
            MACOSX_BUNDLE TRUE
            OUTPUT_NAME "VETTE! 2026 Sound Editor"
            MACOSX_BUNDLE_BUNDLE_NAME "VETTE! 2026 Sound Editor"
            MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.plasticcog.vette2026.sfx"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_ICON_FILE vette2026.icns)
    endif()
    vette_warnings(vette_sfx)
endif()

# --- Tests ---------------------------------------------------------------------------------------
# vette_tests runs everything. Per-component targets (vette_cpu_tests, vette_ega_tests) compile only
# their own sources, so components under parallel development don't break each other's builds.
if(VETTE_BUILD_TESTS)
    file(GLOB VETTE_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
    list(FILTER VETTE_TEST_SOURCES EXCLUDE REGEX "/enhanced_[^/]*\\.cpp$")  # vette_enhanced_tests below
    add_executable(vette_tests ${VETTE_TEST_SOURCES} src/ui/app_icon.cpp)
    target_link_libraries(vette_tests PRIVATE vette_sound vette_core vette_game)
    if(TARGET vette_graphics)
        target_link_libraries(vette_tests PRIVATE vette_graphics)
    endif()
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

    file(GLOB VETTE_ENHANCED_TEST_SOURCES CONFIGURE_DEPENDS tests/enhanced_*.cpp)
    if(VETTE_ENHANCED_TEST_SOURCES)
        add_executable(vette_enhanced_tests tests/test_main.cpp ${VETTE_ENHANCED_TEST_SOURCES})
        target_link_libraries(vette_enhanced_tests PRIVATE vette_enhanced)
        target_include_directories(vette_enhanced_tests PRIVATE tests)
        vette_warnings(vette_enhanced_tests)
    endif()
endif()
