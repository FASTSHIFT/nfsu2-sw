cmake_minimum_required(VERSION 3.20)
project(nv2a_gl C)

# OpenGL 3.3 core renderer for the pushbuffer executor. SDL2 provides the
# window, the context and the entry points (gl_api.c resolves them itself),
# so there is no GL loader dependency and the same code builds for the
# Switch's Mesa.
add_library(xbox_nv2a_gl STATIC
    nv2a_gl.c
    gl_api.c
    gl_vsh.c
    gl_psh.c
)
target_include_directories(xbox_nv2a_gl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    # Pre-built SDL2-devel-*-VC package (has cmake/sdl2-config.cmake), plus the
    # pthread/clock_gettime shim the renderer's Linux-only helpers need.
    set(SDL2_DIR "" CACHE PATH "SDL2-devel-VC cmake dir (contains sdl2-config.cmake)")
    find_package(SDL2 CONFIG REQUIRED)
    target_link_libraries(xbox_nv2a_gl PUBLIC SDL2::SDL2)
    set(_shim "${CMAKE_CURRENT_SOURCE_DIR}/../platform/win32_shim")
    target_include_directories(xbox_nv2a_gl PRIVATE "${_shim}")
    target_sources(xbox_nv2a_gl PRIVATE "${_shim}/pthread.c")
elseif(NINTENDO_SWITCH)
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_nv2a_gl PUBLIC SDL2::SDL2)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    target_link_libraries(xbox_nv2a_gl PUBLIC PkgConfig::SDL2)
endif()
if(NOT MSVC)
    target_compile_options(xbox_nv2a_gl PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()
