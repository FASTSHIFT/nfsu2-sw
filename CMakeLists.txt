cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Need for Speed: Underground 2 (Xbox, NTSC-U) - Static Recompilation
#
# Hosts: Windows (MSVC, D3D11), Linux (GCC, SDL2 + OpenGL) and Nintendo
# Switch homebrew (devkitA64 + libnx, built with switch/build.sh).
# ============================================================================

project(nfsu2_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# The lifted code is ~100 MB of C generated from the retail XBE. It is never
# committed; point this at wherever tools.recomp --gen-dir wrote it.
set(NFSU2_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen" CACHE PATH
    "Directory holding the tools.recomp output (recomp_*.c, recomp_funcs.h)")

if(NOT EXISTS "${NFSU2_GEN_DIR}/recomp_funcs.h")
    message(FATAL_ERROR "No generated code in ${NFSU2_GEN_DIR}. Run tools.recomp "
                        "with --gen-dir, or pass -DNFSU2_GEN_DIR=<dir>.")
endif()

# The Vulkan renderer (xboxrecomp nv2a_vk) instead of OpenGL. On the Switch it
# links mesa-switch's static NVK (NVK_SDK) and builds nfsu2x-vulkan.nro.
option(NFSU2_VULKAN "Render with Vulkan instead of OpenGL" OFF)
set(XBOXRECOMP_VULKAN ${NFSU2_VULKAN} CACHE BOOL "" FORCE)

add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "${NFSU2_GEN_DIR}/*.c")

set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/autotest.c
    src/switch_nx.c
    src/text_patch.c
)

if(WIN32)
    add_executable(${PROJECT_NAME} ${GAME_SOURCES} ${RECOMP_GEN_SOURCES})
else()
    add_executable(${PROJECT_NAME} ${GAME_SOURCES} ${RECOMP_GEN_SOURCES})
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Movies: VP6 decoded by FFmpeg instead of the lifted On2 decoder
# (src/movie_vp6.c). NFSU2_FFMPEG_DIR is an install of the minimal LGPL
# build from tools/build_ffmpeg_vp6.sh (libavcodec with only the vp6
# decoder); without it the lifted decoder plays the movies.
set(NFSU2_FFMPEG_DIR "" CACHE PATH "FFmpeg (vp6 decoder) install prefix")
if(NFSU2_FFMPEG_DIR)
    if(MSVC)
        set(_ffmpeg_codec_lib "${NFSU2_FFMPEG_DIR}/lib/avcodec.lib")
        set(_ffmpeg_util_lib "${NFSU2_FFMPEG_DIR}/lib/avutil.lib")
    else()
        set(_ffmpeg_codec_lib "${NFSU2_FFMPEG_DIR}/lib/libavcodec.a")
        set(_ffmpeg_util_lib "${NFSU2_FFMPEG_DIR}/lib/libavutil.a")
    endif()
    if(NOT EXISTS "${_ffmpeg_codec_lib}")
        message(FATAL_ERROR "no avcodec lib in ${NFSU2_FFMPEG_DIR}/lib")
    endif()
    target_sources(${PROJECT_NAME} PRIVATE src/movie_vp6.c)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NFSU2_NATIVE_VP6=1)
    # BEFORE: the Switch portlibs include dir has devkitPro's (GPL) FFmpeg headers.
    target_include_directories(${PROJECT_NAME} BEFORE PRIVATE "${NFSU2_FFMPEG_DIR}/include")
    target_link_libraries(${PROJECT_NAME} PRIVATE ${_ffmpeg_codec_lib} ${_ffmpeg_util_lib})
endif()

if(NFSU2_VULKAN)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NFSU2_VULKAN=1)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${NFSU2_GEN_DIR}
    ${XBOXRECOMP_DIR}/include
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        d3d11 dxgi dxguid xinput winmm dbghelp)
    if(MSVC)
        target_link_options(${PROJECT_NAME} PRIVATE /LARGEADDRESSAWARE)
        target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
    endif()
else()
    # The generated chunks are huge straight-line functions; -O1 keeps GCC's
    # memory and time per chunk sane while still allocating registers. The
    # Switch's CPU is the bottleneck there, so it gets -O2.
    if(NINTENDO_SWITCH OR XBOXRECOMP_EMBEDDED)
        set(NFSU2_GEN_OPT "-O2" CACHE STRING "Optimisation for the lifted code")
    else()
        set(NFSU2_GEN_OPT "-O1" CACHE STRING "Optimisation for the lifted code")
    endif()
    set(_gen_opts "${NFSU2_GEN_OPT};-w;-fno-strict-aliasing;-fwrapv")

    # Guest memory accesses are volatile by default (recomp_types.h): a poll
    # loop that re-reads a flag must not have its load hoisted. Loops the
    # spin-hint pass marks call out every few turns and stay correct without
    # it; an unmarked one would spin forever -- a hang, not corruption. On x86
    # dropping it measured nothing; on an in-order A35, where 55-65 % of the
    # lifted instructions are loads/stores, it is worth measuring.
    option(NFSU2_GEN_NONVOLATILE "Lifted code: plain (non-volatile) guest memory accesses" OFF)
    if(NFSU2_GEN_NONVOLATILE)
        list(APPEND _gen_opts "-DRECOMP_MEM_VOLATILE=")
    endif()

    # LTO over the lifted code and the game sources: ~19.5k functions in 79
    # files call each other, and without LTO none of those calls can be
    # inlined. nfsmw-nx measured +12 % with LTO+PGO on a 144 MB recompiled
    # game (docs/03). The runtime (xboxrecomp) stays out. -flto-partition and
    # the job count keep the link inside a 10 GB build host.
    option(NFSU2_LTO "LTO over the lifted code and src/" OFF)
    set(NFSU2_LTO_JOBS "4" CACHE STRING "Parallel LTO link jobs")
    if(NFSU2_LTO)
        list(APPEND _gen_opts "-flto" "-fno-fat-lto-objects")
        set_source_files_properties(${GAME_SOURCES} PROPERTIES
            COMPILE_OPTIONS "-flto;-fno-fat-lto-objects")
        target_link_options(${PROJECT_NAME} PRIVATE
            "-flto=${NFSU2_LTO_JOBS}" "-flto-partition=balanced"
            "${NFSU2_GEN_OPT}" "-fno-strict-aliasing" "-fwrapv")
    endif()

    # PGO: NFSU2_PGO=gen builds the instrumented binary (writes .gcda files to
    # NFSU2_PGO_DIR on the device); =use builds with the profile read back.
    set(NFSU2_PGO "" CACHE STRING "PGO stage: empty, gen or use")
    set(NFSU2_PGO_DIR "" CACHE PATH "Profile directory (gen: on the device; use: here)")
    if(NFSU2_PGO STREQUAL "gen")
        # -ftest-coverage: .gcno notes next to the objects (compile time only),
        # so gcov can name the functions in the profile (call counts per
        # function for picking rewrite candidates, docs/02).
        list(APPEND _gen_opts "-fprofile-generate=${NFSU2_PGO_DIR}" "-fno-profile-values"
             "-ftest-coverage")
        target_link_options(${PROJECT_NAME} PRIVATE "-fprofile-generate=${NFSU2_PGO_DIR}")
        # The title rarely exits cleanly; SIGUSR1 writes the .gcda (main.c).
        target_compile_definitions(${PROJECT_NAME} PRIVATE NFSU2_PGO_GEN=1)
    elseif(NFSU2_PGO STREQUAL "use")
        list(APPEND _gen_opts "-fprofile-use=${NFSU2_PGO_DIR}" "-fprofile-partial-training"
             "-fprofile-correction" "-Wno-missing-profile")
    endif()

    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${_gen_opts}")
    target_link_libraries(${PROJECT_NAME} PRIVATE m)
endif()

# ── Nintendo Switch homebrew (devkitA64 + libnx) ────────────────
#
# Build with switch/build.sh. The NRO reads the extracted disc from
# sdmc:/switch/nfsu2x/game/ (never an ISO); logs and nfsu2x_env.txt live in
# sdmc:/switch/nfsu2x/.
if(NINTENDO_SWITCH)
    set(NFSU2_SWITCH_APP_NAME "NFS Underground 2 (Xbox recomp)" CACHE STRING
        "Title shown in the homebrew menu")
    set(NFSU2_SWITCH_AUTHOR "nfsu2-xbox" CACHE STRING "Author shown in the homebrew menu")
    set(NFSU2_SWITCH_VERSION "0.3" CACHE STRING "Version shown in the homebrew menu")
    set(NFSU2_SWITCH_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.jpg" CACHE FILEPATH
        "256x256 JPEG icon (libnx default when empty)")

    if(NFSU2_VULKAN)
        # No GL: SDL is only the audio output; NVK comes with nv2a_vk.
        target_link_libraries(${PROJECT_NAME} PRIVATE nx m)
    else()
        # switch-sdl2's GL path is EGL on switch-mesa.
        target_link_libraries(${PROJECT_NAME} PRIVATE EGL glapi drm_nouveau nx m)
    endif()
    # SDL's audio thread gets a real-time priority and audren gaps are
    # counted (switch_nx.c, "SDL's audio thread").
    target_link_options(${PROJECT_NAME} PRIVATE
        "LINKER:--wrap=SDL_SYS_SetThreadPriority"
        "LINKER:--wrap=audrvVoiceAddWaveBuf")
    set_property(SOURCE src/switch_nx.c APPEND PROPERTY
        INCLUDE_DIRECTORIES "${XBOXRECOMP_DIR}/src/apu")
    # switch_nx.c's loading screen asks EGL for the on-screen size.
    target_include_directories(${PROJECT_NAME} PRIVATE "$ENV{DEVKITPRO}/portlibs/switch/include")

    set(NFSU2_NACP "${CMAKE_CURRENT_BINARY_DIR}/nfsu2x.nacp")
    nx_generate_nacp(OUTPUT "${NFSU2_NACP}"
        NAME "${NFSU2_SWITCH_APP_NAME}"
        AUTHOR "${NFSU2_SWITCH_AUTHOR}"
        VERSION "${NFSU2_SWITCH_VERSION}")
    if(NFSU2_SWITCH_ICON)
        nx_create_nro(${PROJECT_NAME} NACP "${NFSU2_NACP}" ICON "${NFSU2_SWITCH_ICON}")
    else()
        nx_create_nro(${PROJECT_NAME} NACP "${NFSU2_NACP}")
    endif()
endif()
