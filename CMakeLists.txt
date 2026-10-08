cmake_minimum_required(VERSION 3.24)
project(vette2026 VERSION 0.4.0 LANGUAGES C CXX)

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
if(WIN32)
    target_link_libraries(vette_host PUBLIC ws2_32)  # src/host/tcp_link.cpp: the development link's sockets
endif()

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

# --- Online play (src/net): two-player races, through a room on the relay server (server/) or straight
# --- between the two games -------------------------------------------------------------------------------
# vette_net_core is the protocols, their state machines, plain sockets and the direct connection (no
# third-party code; unit-tested). vette_net adds the relay server's WebSocket, which needs libcurl,
# fetched and built here with the system's TLS (Schannel on Windows, Secure Transport on macOS, OpenSSL
# on Linux: libssl-dev; pass -DOPENSSL_USE_STATIC_LIBS=ON to link it into the program), and the router's
# port forwarding for direct codes (miniupnpc and libnatpmp, fetched and built here; without them, direct
# codes need the port forwarded by hand). Without libcurl (offline, or no OpenSSL) the game builds without
# online play: vette_net doesn't exist and VETTE_ONLINE isn't defined.
option(VETTE_ONLINE "Online two-player races (fetches and builds libcurl, miniupnpc, libnatpmp)" ON)
# The relay server this build offers unless the player sets another (net/default_server.h, generated):
# e.g. wss://vette2026-relay.<name>.workers.dev. Empty: none (the player types one in).
set(VETTE_DEFAULT_SERVER "" CACHE STRING "The default online server (wss://...), or empty")
if(VETTE_DEFAULT_SERVER AND NOT VETTE_DEFAULT_SERVER MATCHES "^wss?://[A-Za-z0-9._:/%~-]+$")
    message(FATAL_ERROR "VETTE_DEFAULT_SERVER must be a wss:// address, without quotes: '${VETTE_DEFAULT_SERVER}'")
endif()
configure_file(src/net/default_server.h.in ${CMAKE_BINARY_DIR}/generated/net/default_server.h @ONLY)

file(GLOB VETTE_NET_CORE_SOURCES CONFIGURE_DEPENDS src/net/*.cpp)
set(VETTE_NET_SOURCES src/net/connection.cpp src/net/room_link.cpp src/net/port_mapper.cpp src/net/online.cpp)
foreach(source IN LISTS VETTE_NET_SOURCES)
    list(REMOVE_ITEM VETTE_NET_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/${source})
endforeach()
find_package(Threads REQUIRED)
add_library(vette_net_core STATIC ${VETTE_NET_CORE_SOURCES})
target_include_directories(vette_net_core PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(vette_net_core PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(vette_net_core PUBLIC ws2_32 iphlpapi)
endif()
vette_warnings(vette_net_core)
# The settings take the default server from net/default_server.h.
target_include_directories(vette_core PUBLIC ${CMAKE_BINARY_DIR}/generated)

# Downloads a dependency's archive and checks it; `out` is its path, or empty if that failed (with a
# warning: the game then builds without what it's for).
function(vette_download name url sha256 out)
    set(${out} "" PARENT_SCOPE)
    string(TOUPPER ${name} upper)
    if(FETCHCONTENT_SOURCE_DIR_${upper})
        set(${out} ${FETCHCONTENT_SOURCE_DIR_${upper}} PARENT_SCOPE)
        return()
    endif()
    get_filename_component(file ${url} NAME)
    set(archive ${CMAKE_BINARY_DIR}/_downloads/${file})
    set(hash "")
    if(EXISTS ${archive})
        file(SHA256 ${archive} hash)
    endif()
    if(NOT hash STREQUAL sha256)
        file(DOWNLOAD ${url} ${archive} STATUS status)
        list(GET status 0 code)
        if(code EQUAL 0)
            file(SHA256 ${archive} hash)
        endif()
        if(NOT hash STREQUAL sha256)
            file(REMOVE ${archive})
            message(WARNING "${name} couldn't be downloaded (${status}).")
            return()
        endif()
    endif()
    set(${out} ${archive} PARENT_SCOPE)
endfunction()

# libcurl 8.14 is the last release with Secure Transport (macOS's own TLS; removed in 8.15).
set(VETTE_CURL_VERSION 8.14.1)
set(VETTE_CURL_SHA256 f4619a1e2474c4bbfedc88a7c2191209c8334b48fa1f4e53fd584cc12e9120dd)
function(vette_fetch_curl)
    if(NOT WIN32 AND NOT APPLE)
        find_package(OpenSSL)
        if(NOT OPENSSL_FOUND)
            message(WARNING "OpenSSL (libssl-dev) wasn't found: building without online play.")
            return()
        endif()
        if(OPENSSL_USE_STATIC_LIBS)
            # A static libcrypto needs these after it on the link line.
            find_package(Threads REQUIRED)
            set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
                         INTERFACE_LINK_LIBRARIES ${CMAKE_DL_LIBS} Threads::Threads)
        endif()
    endif()
    string(REPLACE "." "_" tag ${VETTE_CURL_VERSION})
    # Downloaded here rather than by FetchContent, so a failure leaves the game buildable.
    vette_download(curl https://github.com/curl/curl/releases/download/curl-${tag}/curl-${VETTE_CURL_VERSION}.tar.xz
                   ${VETTE_CURL_SHA256} source)
    if(NOT source)
        message(WARNING "Building without online play (no libcurl).")
        return()
    endif()

    # Only a TCP/TLS connection is needed (the WebSocket is our own). Normal variables stay in this
    # function's scope; the ones curl reads with set(CACHE) must be cache variables.
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_STATIC_LIBS ON)
    set(BUILD_CURL_EXE OFF)
    set(BUILD_TESTING OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_LIBCURL_DOCS OFF)
    set(BUILD_MISC_DOCS OFF)
    set(ENABLE_CURL_MANUAL OFF)
    set(CURL_DISABLE_INSTALL ON)
    set(CURL_ENABLE_EXPORT_TARGET OFF)
    set(PICKY_COMPILER OFF)
    set(HTTP_ONLY ON)
    foreach(feature WEBSOCKETS ALTSVC HSTS COOKIES DOH NETRC MIME FORM_API AWS NTLM KERBEROS_AUTH
                    NEGOTIATE_AUTH SRP)
        set(CURL_DISABLE_${feature} ON)
    endforeach()
    set(CURL_USE_LIBPSL OFF)
    set(CURL_USE_LIBSSH2 OFF)
    set(CURL_USE_GSSAPI OFF)
    set(USE_LIBIDN2 OFF)
    set(USE_NGHTTP2 OFF)
    foreach(dependency ZLIB BROTLI ZSTD)
        set(CURL_${dependency} OFF CACHE STRING "" FORCE)
    endforeach()
    set(ENABLE_UNIX_SOCKETS OFF)
    if(WIN32)
        set(CURL_USE_SCHANNEL ON)
        set(CURL_USE_OPENSSL OFF)
    elseif(APPLE)
        set(CURL_USE_SECTRANSP ON)
        set(CURL_USE_OPENSSL OFF)
    else()
        set(CURL_USE_OPENSSL ON)
        # The CA certificates are found at run time (src/net/connection.cpp): distributions keep them in
        # different places, and the build machine's paths would be baked in otherwise. Without them,
        # OpenSSL's own default applies.
        set(CURL_CA_BUNDLE none CACHE STRING "" FORCE)
        set(CURL_CA_PATH none CACHE STRING "" FORCE)
        set(CURL_CA_FALLBACK ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_OPENSSL_AUTO_LOAD_CONFIG ON)
    endif()
    FetchContent_Declare(curl URL ${source} URL_HASH SHA256=${VETTE_CURL_SHA256} DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(curl)
endfunction()

# Port forwarding on the home router for direct codes: miniupnpc (UPnP IGD) and libnatpmp (NAT-PMP, and the
# default gateway for PCP), both BSD 3-Clause, by Thomas Bernard. Built here from their sources.
set(VETTE_MINIUPNPC_VERSION 2.3.3)
set(VETTE_MINIUPNPC_SHA256 d52a0afa614ad6c088cc9ddff1ae7d29c8c595ac5fdd321170a05f41e634bd1a)
set(VETTE_NATPMP_COMMIT 134fc89e2781e154e40042641f4d8bcbe42579f1)  # 2025-04-03
set(VETTE_NATPMP_SHA256 be1864ed4e54a122590fd2de3824b085386b6e4aa9a91738c277c280ab8bf188)
function(vette_fetch_port_mapping)
    string(REPLACE "." "_" tag ${VETTE_MINIUPNPC_VERSION})
    vette_download(miniupnpc
        https://github.com/miniupnp/miniupnp/releases/download/miniupnpc_${tag}/miniupnpc-${VETTE_MINIUPNPC_VERSION}.tar.gz
        ${VETTE_MINIUPNPC_SHA256} archive)
    if(archive)
        # SOURCE_SUBDIR without a CMakeLists.txt: only unpacked; the library is defined here.
        FetchContent_Declare(miniupnpc URL ${archive} URL_HASH SHA256=${VETTE_MINIUPNPC_SHA256}
                             SOURCE_SUBDIR unused DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(miniupnpc)
        set(src ${miniupnpc_SOURCE_DIR})
        add_library(vette_miniupnpc STATIC
            ${src}/src/igd_desc_parse.c ${src}/src/miniupnpc.c ${src}/src/minixml.c ${src}/src/minisoap.c
            ${src}/src/minissdpc.c ${src}/src/miniwget.c ${src}/src/upnpcommands.c ${src}/src/upnpdev.c
            ${src}/src/upnpreplyparse.c ${src}/src/upnperrors.c ${src}/src/connecthostport.c
            ${src}/src/portlistingparse.c ${src}/src/receivedata.c ${src}/src/addr_is_reserved.c)
        set(OS_STRING ${CMAKE_SYSTEM_NAME})
        set(PROJECT_VERSION ${VETTE_MINIUPNPC_VERSION})
        configure_file(${src}/miniupnpcstrings.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/miniupnpc/miniupnpcstrings.h)
        target_include_directories(vette_miniupnpc SYSTEM PUBLIC ${src}/include)
        target_include_directories(vette_miniupnpc PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/miniupnpc)
        target_compile_definitions(vette_miniupnpc PUBLIC MINIUPNP_STATICLIB)
        if(WIN32)
            target_compile_definitions(vette_miniupnpc PRIVATE _WIN32_WINNT=0x0601 _CRT_SECURE_NO_WARNINGS
                                       _WINSOCK_DEPRECATED_NO_WARNINGS)
            target_link_libraries(vette_miniupnpc PUBLIC ws2_32 iphlpapi)
        else()
            target_compile_definitions(vette_miniupnpc PRIVATE MINIUPNPC_SET_SOCKET_TIMEOUT MINIUPNPC_GET_SRC_ADDR
                                       _BSD_SOURCE _DEFAULT_SOURCE)
            if(APPLE)
                target_compile_definitions(vette_miniupnpc PRIVATE _DARWIN_C_SOURCE)
            elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                target_compile_definitions(vette_miniupnpc PRIVATE _XOPEN_SOURCE=600)
            endif()
        endif()
        target_compile_options(vette_miniupnpc PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/W0,-w>)  # third-party
    endif()

    vette_download(natpmp https://github.com/miniupnp/libnatpmp/archive/${VETTE_NATPMP_COMMIT}.tar.gz
                   ${VETTE_NATPMP_SHA256} archive)
    if(archive)
        FetchContent_Declare(natpmp URL ${archive} URL_HASH SHA256=${VETTE_NATPMP_SHA256}
                             SOURCE_SUBDIR unused DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(natpmp)
        set(src ${natpmp_SOURCE_DIR})
        add_library(vette_natpmp STATIC ${src}/natpmp.c ${src}/getgateway.c)
        if(WIN32)
            target_sources(vette_natpmp PRIVATE ${src}/wingettimeofday.c)
            target_link_libraries(vette_natpmp PUBLIC ws2_32 iphlpapi)
        endif()
        # Only the public headers, from a folder of their own: the source folder has a file named VERSION,
        # which a case-insensitive file system (macOS) takes for C++20's <version>.
        file(COPY ${src}/natpmp.h ${src}/natpmp_declspec.h DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/natpmp/include)
        target_include_directories(vette_natpmp SYSTEM PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/natpmp/include)
        target_include_directories(vette_natpmp PRIVATE ${src})
        target_compile_definitions(vette_natpmp PUBLIC NATPMP_STATICLIB PRIVATE ENABLE_STRNATPMPERR
                                   NATPMP_MAX_RETRIES=4)
        target_compile_options(vette_natpmp PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/W0,-w>)  # third-party
    endif()
endfunction()

if(VETTE_ONLINE)
    vette_fetch_curl()
    if(TARGET libcurl_static)
        vette_fetch_port_mapping()
    endif()
endif()
if(TARGET libcurl_static)
    foreach(curl_target libcurl_object libcurl_static)  # third-party (under MSVC it builds clean at /W4)
        if(TARGET ${curl_target} AND NOT MSVC)
            target_compile_options(${curl_target} PRIVATE -w)
        endif()
    endforeach()
    add_library(vette_net STATIC ${VETTE_NET_SOURCES})
    target_link_libraries(vette_net PUBLIC vette_net_core PRIVATE libcurl_static)
    if(TARGET vette_miniupnpc)
        target_link_libraries(vette_net PRIVATE vette_miniupnpc)
        target_compile_definitions(vette_net PRIVATE VETTE_HAVE_MINIUPNPC=1)
    endif()
    if(TARGET vette_natpmp)
        target_link_libraries(vette_net PRIVATE vette_natpmp)
        target_compile_definitions(vette_net PRIVATE VETTE_HAVE_NATPMP=1)
    endif()
    if(WIN32)
        target_link_libraries(vette_net PRIVATE winmm)
    endif()
    target_compile_definitions(vette_net PUBLIC VETTE_ONLINE=1)
    vette_warnings(vette_net)
endif()

# Headless runner: boots the hosted game, injects keys, saves screenshots (no SDL needed).
vette_tool(vette_run vette_game vette_core)
# Two hosted games linked by an in-memory serial cable: the two-player race without a network.
vette_tool(vette_link vette_game)
# Differential fuzzer: calls each native port with random inputs under Verify mode.
vette_tool(vette_fuzz vette_game)
# World extraction tool: dumps the Enhanced world model and validates it against the original.
vette_tool(vette_world vette_enhanced vette_game vette_core)
# The other versions' files: Mac resources and sounds, PC-98 disk files and FM sound, art.
vette_tool(vette_mac vette_sound)
vette_tool(vette_pc98 vette_sound)
if(TARGET vette_graphics)
    vette_tool(vette_gfx vette_graphics)
endif()
# Online play check: hosts (room code, direct code) or joins, sends test bytes, prints round-trip times.
if(TARGET vette_net)
    vette_tool(vette_netcheck vette_net)
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
        src/ui/key_sheet.cpp
        src/ui/launcher.cpp
        src/ui/map_editor.cpp
        src/ui/online.cpp
        src/platform/url_scheme.cpp
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
            MACOSX_BUNDLE_ICON_FILE vette2026.icns
            # CMake's own template plus the vette2026:// invite links (platform/url_scheme.h).
            MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/MacOSXBundleInfo.plist.in)
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
    if(TARGET vette_net)
        target_link_libraries(vette2026 PRIVATE vette_net)  # defines VETTE_ONLINE
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
    list(FILTER VETTE_TEST_SOURCES EXCLUDE REGEX "/online_[^/]*\\.cpp$")  # vette_online_tests below
    list(FILTER VETTE_TEST_SOURCES EXCLUDE REGEX "/enhanced_[^/]*\\.cpp$")  # vette_enhanced_tests below
    add_executable(vette_tests ${VETTE_TEST_SOURCES} src/ui/app_icon.cpp src/ui/canvas.cpp src/ui/key_sheet.cpp)
    target_link_libraries(vette_tests PRIVATE vette_sound vette_core vette_game vette_net_core)
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
    vette_component_tests(net)
    if(TARGET vette_net_tests)
        target_link_libraries(vette_net_tests PRIVATE vette_net_core)
    endif()
    # Hosting and joining through net::OnlineLink (needs vette_net, so only when it's built).
    if(TARGET vette_net)
        file(GLOB VETTE_ONLINE_TEST_SOURCES CONFIGURE_DEPENDS tests/online_*.cpp)
        add_executable(vette_online_tests tests/test_main.cpp ${VETTE_ONLINE_TEST_SOURCES})
        target_link_libraries(vette_online_tests PRIVATE vette_net)
        target_include_directories(vette_online_tests PRIVATE tests)
        vette_warnings(vette_online_tests)
    endif()

    file(GLOB VETTE_ENHANCED_TEST_SOURCES CONFIGURE_DEPENDS tests/enhanced_*.cpp)
    if(VETTE_ENHANCED_TEST_SOURCES)
        add_executable(vette_enhanced_tests tests/test_main.cpp ${VETTE_ENHANCED_TEST_SOURCES})
        target_link_libraries(vette_enhanced_tests PRIVATE vette_enhanced)
        target_include_directories(vette_enhanced_tests PRIVATE tests)
        vette_warnings(vette_enhanced_tests)
    endif()
endif()
