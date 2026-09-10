cmake_minimum_required(VERSION 3.22.1)
project("imgui_overlay")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui-1.92.9b)

# ---------------------------------------------------------------------------
# Vulkan: compile the GLSL shaders to SPIR-V with the glslc that ships inside
# the NDK, then embed the binaries as C arrays (see embed_spv.cmake).
# ---------------------------------------------------------------------------
set(VK_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/spv)
file(MAKE_DIRECTORY ${VK_GEN_DIR})

set(_vk_ndk_roots "")
if (CMAKE_ANDROID_NDK)
    list(APPEND _vk_ndk_roots "${CMAKE_ANDROID_NDK}")
endif ()
if (ANDROID_NDK)
    list(APPEND _vk_ndk_roots "${ANDROID_NDK}")
endif ()
if (DEFINED ENV{ANDROID_NDK_ROOT})
    list(APPEND _vk_ndk_roots "$ENV{ANDROID_NDK_ROOT}")
endif ()
if (DEFINED ENV{ANDROID_NDK_HOME})
    list(APPEND _vk_ndk_roots "$ENV{ANDROID_NDK_HOME}")
endif ()
if (DEFINED ENV{ANDROID_HOME})
    list(APPEND _vk_ndk_roots "$ENV{ANDROID_HOME}/ndk")
endif ()

set(VK_GLSLC "")
foreach (_root ${_vk_ndk_roots})
    file(GLOB _vk_candidates
            "${_root}/shader-tools/*/glslc"
            "${_root}/shader-tools/*/glslc.exe"
            "${_root}/*/shader-tools/*/glslc"
            "${_root}/*/shader-tools/*/glslc.exe")
    if (_vk_candidates)
        list(GET _vk_candidates 0 VK_GLSLC)
        break()
    endif ()
endforeach ()

if (NOT VK_GLSLC)
    message(FATAL_ERROR "glslc not found. The NDK ships it under "
            "<ndk>/shader-tools/<host>/. Searched: ${_vk_ndk_roots}")
endif ()
message(STATUS "Vulkan shader compiler: ${VK_GLSLC}")

function(add_spv_embedded_header name stage src array)
    set(_txt "${VK_GEN_DIR}/${name}.txt")
    set(_hdr "${VK_GEN_DIR}/${name}_spv.h")
    add_custom_command(
            OUTPUT "${_hdr}"
            COMMAND "${VK_GLSLC}" -fshader-stage=${stage} --target-env=vulkan1.1
                    -mfmt=c -o "${_txt}" "${src}"
            COMMAND "${CMAKE_COMMAND}" -DSPV_INPUT=${_txt} -DSPV_OUTPUT=${_hdr}
                    -DSPV_ARRAY=${array} -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spv.cmake"
            DEPENDS "${src}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_spv.cmake"
            COMMENT "Compiling ${src} to SPIR-V (Vulkan 1.1)"
            VERBATIM)
endfunction()

add_spv_embedded_header(solid_vert vert
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/solid.vert" kSolidVertSpv)
add_spv_embedded_header(solid_frag frag
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/solid.frag" kSolidFragSpv)

add_library(imgui_overlay SHARED
        imgui_overlay.cpp
        solid_geometry.cpp
        platonic_solid.cpp
        cpu_solid.cpp
        vulkan_solid.cpp
        angle_solid.cpp
        ${VK_GEN_DIR}/solid_vert_spv.h
        ${VK_GEN_DIR}/solid_frag_spv.h
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui_overlay PRIVATE
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${VK_GEN_DIR}
)

find_library(vulkan-lib vulkan)

target_link_libraries(imgui_overlay
        android
        log
        EGL
        GLESv3
        ${vulkan-lib}
)
