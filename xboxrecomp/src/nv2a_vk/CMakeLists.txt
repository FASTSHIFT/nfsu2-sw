cmake_minimum_required(VERSION 3.20)
project(nv2a_vk C)
# Vulkan renderer for the pushbuffer executor (XBOXRECOMP_VULKAN=ON): the
# Switch's NVK from mesa-switch, linked statically (NVK_SDK), or any Vulkan
# driver on Linux -- 1.3 with push descriptors and dynamic vertex input, or a
# plain 1.0 driver (the R36S's Mali blob; nv2a_vk.c "Vulkan 1.0 drivers").
# Shares the shader generators with nv2a_gl, in their Vulkan dialect, and
# compiles them at run time with glslang -- or, XBOXRECOMP_VK_DLOPEN=ON, with
# the device's libshaderc.so.1.
add_library(xbox_nv2a_vk STATIC
    nv2a_vk.c
    ../nv2a_gl/gl_vsh.c
    ../nv2a_gl/gl_psh.c
)
target_include_directories(xbox_nv2a_vk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(TARGET platform)
    target_link_libraries(xbox_nv2a_vk PUBLIC platform)    # platform/xtrace.h
endif()
# XBOXRECOMP_VK_DLOPEN: libvulkan.so.1 and libshaderc.so.1 are opened at run
# time; the build needs only the Vulkan headers (XBOXRECOMP_VK_HEADERS: a
# directory holding vulkan/vulkan.h, when the toolchain has none) -- the
# R36S's cross image has no arm64 Vulkan or shaderc to link against.
option(XBOXRECOMP_VK_DLOPEN "Open libvulkan and libshaderc at run time (no glslang)" OFF)
set(XBOXRECOMP_VK_HEADERS "" CACHE PATH "Directory with vulkan/vulkan.h (XBOXRECOMP_VK_DLOPEN builds)")
if(XBOXRECOMP_VK_DLOPEN)
    target_compile_definitions(xbox_nv2a_vk PRIVATE NV2A_VK_DLOPEN=1)
    if(XBOXRECOMP_VK_HEADERS)
        target_include_directories(xbox_nv2a_vk PRIVATE "${XBOXRECOMP_VK_HEADERS}")
    endif()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    target_link_libraries(xbox_nv2a_vk PUBLIC PkgConfig::SDL2 ${CMAKE_DL_LIBS} pthread)
elseif(NINTENDO_SWITCH)
    find_package(glslang CONFIG REQUIRED)
    target_link_libraries(xbox_nv2a_vk PUBLIC glslang::glslang glslang::glslang-default-resource-limits)
    set(NVK_SDK "" CACHE PATH "mesa-switch install prefix (holds lib/libvulkan.a, include/vulkan)")
    if(NOT EXISTS "${NVK_SDK}/lib/libvulkan.a")
        message(FATAL_ERROR "NVK_SDK must point at a mesa-switch install (no lib/libvulkan.a in '${NVK_SDK}')")
    endif()
    target_include_directories(xbox_nv2a_vk PUBLIC "${NVK_SDK}/include")
    target_sources(xbox_nv2a_vk PRIVATE switch_elf_stubs.c switch_egl_stubs.c)
    # Static NVK and what it needs; listed twice for archive rescanning.
    target_link_libraries(xbox_nv2a_vk PUBLIC
        "${NVK_SDK}/lib/libvulkan.a" expat z nx
        "${NVK_SDK}/lib/libvulkan.a" expat z nx stdc++ m)
else()
    find_package(glslang CONFIG REQUIRED)
    target_link_libraries(xbox_nv2a_vk PUBLIC glslang::glslang glslang::glslang-default-resource-limits)
    find_package(Vulkan REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    # The distribution glslang is built with its optimizer (static SPIRV-Tools).
    target_link_libraries(xbox_nv2a_vk PUBLIC Vulkan::Vulkan PkgConfig::SDL2
        SPIRV-Tools-opt SPIRV-Tools stdc++)
endif()
if(NOT MSVC)
    target_compile_options(xbox_nv2a_vk PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -O2)
endif()
