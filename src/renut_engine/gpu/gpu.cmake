# reNut native renderer: plume (D3D12 + Vulkan), host shaders, and the Xenos
# shader cache produced by XenosRecomp (RENUT_RECOMP only).
#
# Included from the top-level CMakeLists.txt after the renut target exists:
#   include(src/renut_engine/gpu/gpu.cmake)
#   renut_gpu_setup(renut)

include(ExternalProject)

set(RENUT_GPU_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(RENUT_XENOS_RECOMP_DIR "${CMAKE_SOURCE_DIR}/thirdparty/reblue-XenosRecomp")
set(RENUT_SHADER_COMMON_DIR "${RENUT_XENOS_RECOMP_DIR}/XenosRecomp")
set(RENUT_GPU_GEN_DIR "${CMAKE_BINARY_DIR}/renut_gpu")

set(RENUT_SHADER_DUMP_DIR "${CMAKE_SOURCE_DIR}/shaders" CACHE PATH
    "Directory of dumped Xenos shader containers (tools/dump_shaders.py output)")
set(RENUT_ASSETS_DIR "${CMAKE_SOURCE_DIR}/assets" CACHE PATH
    "Game assets the shaders are dumped from (default.xex and Bundle/)")
set(RENUT_SHADER_CAPTURE_DIR "${CMAKE_BINARY_DIR}/shader_capture" CACHE PATH
    "Where reNut, run from the build directory, captures shaders its cache lacks "
    "(renut_shader_capture_dir); the build adds them to RENUT_SHADER_DUMP_DIR")

# tools/dump_shaders.py, which runs when there are no dumped shaders yet
# (renut_xenos_shader_cache), needs Python 3 with the xxhash module.
if(NOT EXISTS "${RENUT_SHADER_DUMP_DIR}")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import xxhash"
                    RESULT_VARIABLE renut_xxhash_missing OUTPUT_QUIET ERROR_QUIET)
    if(renut_xxhash_missing)
        message(FATAL_ERROR "tools/dump_shaders.py needs the Python xxhash module: "
                            "${Python3_EXECUTABLE} -m pip install xxhash")
    endif()
endif()

# plume is read-only and vendored; build it as-is.
# On Linux its Vulkan surface goes through SDL (PLUME_SDL_VULKAN_ENABLED), so
# Wayland works as well as X11: plume takes its <SDL_vulkan.h> from the
# SDL2::SDL2 target, which here is reNut's SDL2-to-SDL3 bridge
# (backend/sdl2_vulkan) over the SDK's SDL3 headers; the surface itself is
# made in backend/native_window.cpp.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT TARGET plume)
    set(PLUME_SDL_VULKAN_ENABLED ON CACHE BOOL "plume's Vulkan surface through SDL (Wayland and X11)" FORCE)
    if(NOT TARGET SDL2::SDL2)
        add_library(SDL2::SDL2 INTERFACE IMPORTED)
        set_target_properties(SDL2::SDL2 PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
            "${RENUT_GPU_DIR}/backend/sdl2_vulkan;$<TARGET_PROPERTY:SDL3::SDL3-static,INTERFACE_INCLUDE_DIRECTORIES>")
    endif()
endif()
if(NOT TARGET plume)
    add_subdirectory("${CMAKE_SOURCE_DIR}/thirdparty/plume" "${CMAKE_BINARY_DIR}/plume")
endif()


# The shader cache is zstd-compressed and its SPIR-V is smol-v encoded. Only the
# decoders are needed, built from the copies vendored with XenosRecomp.
enable_language(C)
set(RENUT_ZSTD_LIB_DIR "${RENUT_XENOS_RECOMP_DIR}/thirdparty/zstd/lib")
file(GLOB renut_zstd_sources
    "${RENUT_ZSTD_LIB_DIR}/common/*.c"
    "${RENUT_ZSTD_LIB_DIR}/decompress/*.c")
add_library(renut_zstd STATIC ${renut_zstd_sources})
target_include_directories(renut_zstd PUBLIC "${RENUT_ZSTD_LIB_DIR}")
target_compile_definitions(renut_zstd PRIVATE ZSTD_DISABLE_ASM)
set_target_properties(renut_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Shader containers are keyed by XXH3_64, hashed with the same xxHash
# XenosRecomp uses.
set(RENUT_XXHASH_DIR "${RENUT_XENOS_RECOMP_DIR}/thirdparty/xxHash")
add_library(renut_xxhash STATIC "${RENUT_XXHASH_DIR}/xxhash.c")
target_include_directories(renut_xxhash PUBLIC "${RENUT_XXHASH_DIR}")
set_target_properties(renut_xxhash PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(RENUT_SMOLV_DIR "${RENUT_XENOS_RECOMP_DIR}/thirdparty/smol-v/source")
add_library(renut_smolv STATIC "${RENUT_SMOLV_DIR}/smolv.cpp")
target_include_directories(renut_smolv PUBLIC "${RENUT_SMOLV_DIR}")
set_target_properties(renut_smolv PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# DXC: the copy vendored with XenosRecomp covers Windows and Linux.
# ---------------------------------------------------------------------------
set(RENUT_DXC_BIN_DIR "${RENUT_XENOS_RECOMP_DIR}/thirdparty/dxc-bin/bin/x64")
if(WIN32)
    set(RENUT_DXC "${RENUT_DXC_BIN_DIR}/dxc.exe")
else()
    # The Linux binary finds libdxcompiler next to it.
    set(RENUT_DXC env "LD_LIBRARY_PATH=${RENUT_XENOS_RECOMP_DIR}/thirdparty/dxc-bin/lib/x64" "${RENUT_DXC_BIN_DIR}/dxc-linux")
endif()

# renut_host_shader(<target> <stem> <profile>) compiles gpu/shaders/host/<stem>.hlsl
# to SPIR-V (always) and DXIL (Windows), as headers exposing
# g_<stem>_spirv / g_<stem>_dxil.
function(renut_host_shader TARGET STEM PROFILE)
    set(src "${RENUT_GPU_DIR}/shaders/host/${STEM}.hlsl")
    file(GLOB host_includes "${RENUT_GPU_DIR}/shaders/host/*.hlsli")
    # smaa_common.hlsli includes SMAA's own.
    list(APPEND host_includes "${CMAKE_SOURCE_DIR}/thirdparty/smaa/SMAA.hlsl")

    set(formats spirv)
    if(WIN32)
        list(APPEND formats dxil)
    endif()

    foreach(fmt IN LISTS formats)
        set(format_args "")
        if(fmt STREQUAL "spirv")
            # Same cbuffer layout as D3D, and D3D clip space in vertex shaders.
            set(format_args -spirv -fvk-use-dx-layout)
            if(PROFILE MATCHES "^vs")
                list(APPEND format_args -fvk-invert-y)
            endif()
        endif()
        set(out "${RENUT_GPU_GEN_DIR}/host_shaders/${STEM}.${fmt}.h")
        add_custom_command(
            OUTPUT "${out}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${RENUT_GPU_GEN_DIR}/host_shaders"
            COMMAND ${RENUT_DXC} -T ${PROFILE} -E main -HV 2021 -all-resources-bound
                    -Wno-ignored-attributes ${format_args}
                    -Fh "${out}" -Vn g_${STEM}_${fmt} "${src}"
            DEPENDS "${src}" ${host_includes}
            COMMENT "Compiling host shader ${STEM}.hlsl (${PROFILE}, ${fmt})"
            VERBATIM)
        target_sources(${TARGET} PRIVATE "${out}")
    endforeach()
endfunction()

# ---------------------------------------------------------------------------
# Xenos shader cache, in three steps:
#  1. tools/dump_shaders.py dumps every shader container from the game's
#     default.xex and bundles (RENUT_ASSETS_DIR) into RENUT_SHADER_DUMP_DIR,
#     only when that directory does not exist. An existing one is used as it
#     is, containers added by hand included; delete it to dump again.
#  2. The shaders reNut captured at runtime (RENUT_SHADER_CAPTURE_DIR: the ones
#     its cache lacked, which the game compiles itself and no dump has) are
#     copied into the dump directory, those it does not have yet. Then
#     XenosRecomp, built standalone with RENUT_RECOMP, recompiles every .bin
#     there into shader_cache.cpp (DXIL and SPIR-V, zstd-compressed).
#  3. shader_cache.cpp is compiled into the executable.
# ---------------------------------------------------------------------------
function(renut_xenos_shader_cache TARGET)
    set(recomp_prefix "${CMAKE_BINARY_DIR}/xenos_recomp")
    set(recomp_exe "${recomp_prefix}/build/XenosRecomp/XenosRecomp${CMAKE_EXECUTABLE_SUFFIX}")
    set(recomp_dxil ON)
    if(NOT WIN32)
        set(recomp_dxil OFF)
    endif()

    # XenosRecomp runs during the build, so a cross build (Android) compiles it
    # for the host.
    set(recomp_cc "${CMAKE_C_COMPILER}")
    set(recomp_cxx "${CMAKE_CXX_COMPILER}")
    if(CMAKE_CROSSCOMPILING)
        set(RENUT_HOST_C_COMPILER clang CACHE STRING "Host C compiler for build tools")
        set(RENUT_HOST_CXX_COMPILER clang++ CACHE STRING "Host C++ compiler for build tools")
        set(recomp_cc "${RENUT_HOST_C_COMPILER}")
        set(recomp_cxx "${RENUT_HOST_CXX_COMPILER}")
        set(recomp_exe "${recomp_prefix}/build/XenosRecomp/XenosRecomp")
    endif()

    ExternalProject_Add(renut_xenos_recomp
        SOURCE_DIR "${RENUT_XENOS_RECOMP_DIR}"
        BINARY_DIR "${recomp_prefix}/build"
        PREFIX "${recomp_prefix}"
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_C_COMPILER=${recomp_cc}
            -DCMAKE_CXX_COMPILER=${recomp_cxx}
            -DXENOS_RECOMP_RENUT=ON
            -DXENOS_RECOMP_DXIL=${recomp_dxil}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target XenosRecomp --config Release
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${recomp_exe}"
        # Rebuild whenever its sources change (a no-op otherwise): the cache
        # layout comes from it.
        BUILD_ALWAYS ON
        USES_TERMINAL_BUILD ON)

    # 1. Dump, only when the dump directory does not exist yet (a fresh
    # checkout): the dump reads gigabytes of bundles, and an existing
    # directory is used as it is. Delete it to dump again.
    set(dump_outputs "")
    if(NOT EXISTS "${RENUT_SHADER_DUMP_DIR}")
        if(NOT EXISTS "${RENUT_ASSETS_DIR}/default.xex")
            message(FATAL_ERROR "No ${RENUT_ASSETS_DIR}/default.xex to dump shaders from: set "
                                "RENUT_ASSETS_DIR to the game's assets directory")
        endif()
        set(dump_outputs "${RENUT_SHADER_DUMP_DIR}/manifest.csv")
        add_custom_command(
            OUTPUT ${dump_outputs}
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/dump_shaders.py"
                    --allow-problems --assets "${RENUT_ASSETS_DIR}" --out "${RENUT_SHADER_DUMP_DIR}"
            COMMENT "Dumping Xenos shaders from ${RENUT_ASSETS_DIR} to ${RENUT_SHADER_DUMP_DIR}"
            USES_TERMINAL VERBATIM)
    endif()

    # 2. Add the captures, then recompile; after the dump when there is one.
    # The globs pick up the containers already there and new captures (a
    # capture the dump directory already has changes nothing).
    set(cache_cpp "${RENUT_GPU_GEN_DIR}/shader_cache.cpp")
    file(GLOB shader_inputs CONFIGURE_DEPENDS "${RENUT_SHADER_DUMP_DIR}/*.bin")
    file(GLOB captured_inputs CONFIGURE_DEPENDS "${RENUT_SHADER_CAPTURE_DIR}/*.bin")
    add_custom_command(
        OUTPUT "${cache_cpp}"
        COMMAND "${CMAKE_COMMAND}" "-DCAPTURE_DIR=${RENUT_SHADER_CAPTURE_DIR}"
                "-DDUMP_DIR=${RENUT_SHADER_DUMP_DIR}"
                -P "${CMAKE_SOURCE_DIR}/cmake/collect_captured_shaders.cmake"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RENUT_GPU_GEN_DIR}"
        COMMAND "${recomp_exe}" "${RENUT_SHADER_DUMP_DIR}" "${cache_cpp}"
                "${RENUT_SHADER_COMMON_DIR}/shader_common.h"
        DEPENDS renut_xenos_recomp "${recomp_exe}" "${RENUT_SHADER_COMMON_DIR}/shader_common.h"
                "${CMAKE_SOURCE_DIR}/cmake/collect_captured_shaders.cmake"
                ${dump_outputs} ${shader_inputs} ${captured_inputs}
        COMMENT "Recompiling Xenos shaders (RENUT_RECOMP) from ${RENUT_SHADER_DUMP_DIR}"
        USES_TERMINAL VERBATIM)

    # 3. Compile into the executable.
    add_custom_target(renut_shader_cache_gen DEPENDS "${cache_cpp}")
    add_dependencies(${TARGET} renut_shader_cache_gen)
    target_sources(${TARGET} PRIVATE "${cache_cpp}")
    # The generated file is large and only holds byte arrays.
    set_source_files_properties("${cache_cpp}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endfunction()

# ---------------------------------------------------------------------------
function(renut_gpu_setup TARGET)
    target_sources(${TARGET} PRIVATE
        # The host device, the frame and the render thread.
        ${RENUT_GPU_DIR}/backend/backend.cpp
        ${RENUT_GPU_DIR}/backend/command_recorder.cpp
        ${RENUT_GPU_DIR}/backend/gpu_profile.cpp
        ${RENUT_GPU_DIR}/backend/memory_report.cpp
        ${RENUT_GPU_DIR}/backend/native_window.cpp
        ${RENUT_GPU_DIR}/backend/present.cpp
        ${RENUT_GPU_DIR}/backend/queued.cpp
        ${RENUT_GPU_DIR}/backend/render_profile.cpp
        ${RENUT_GPU_DIR}/backend/render_thread.cpp
        ${RENUT_GPU_DIR}/backend/smaa.cpp
        ${RENUT_GPU_DIR}/backend/video.cpp
        # The guest D3D runtime, native, and its hooks.
        ${RENUT_GPU_DIR}/d3d/d3d_native_state.cpp
        ${RENUT_GPU_DIR}/d3d/hooks/device.cpp
        ${RENUT_GPU_DIR}/d3d/hooks/draw.cpp
        ${RENUT_GPU_DIR}/d3d/hooks/resource.cpp
        ${RENUT_GPU_DIR}/d3d/hooks/shader.cpp
        ${RENUT_GPU_DIR}/d3d/hooks/state.cpp
        # Guest-side records, command buffers and callbacks.
        ${RENUT_GPU_DIR}/guest/callbacks.cpp
        ${RENUT_GPU_DIR}/guest/command_buffers.cpp
        ${RENUT_GPU_DIR}/guest/guest_records.cpp
        ${RENUT_GPU_DIR}/guest/photo_capture.cpp
        ${RENUT_GPU_DIR}/guest/widescreen.cpp
        # reNut's overlay and the SDK's menus.
        ${RENUT_GPU_DIR}/overlay/app_integration.cpp
        ${RENUT_GPU_DIR}/overlay/overlay_drawer.cpp
        ${RENUT_GPU_DIR}/overlay/overlay_ui.cpp
        # Draws, targets, textures, shaders and formats.
        ${RENUT_GPU_DIR}/render/draw.cpp
        ${RENUT_GPU_DIR}/render/format.cpp
        ${RENUT_GPU_DIR}/render/graphics_options.cpp
        ${RENUT_GPU_DIR}/render/parallel.cpp
        ${RENUT_GPU_DIR}/render/pipeline.cpp
        ${RENUT_GPU_DIR}/render/shader_library.cpp
        ${RENUT_GPU_DIR}/render/surfaces.cpp
        ${RENUT_GPU_DIR}/render/texture_layout.cpp
        ${RENUT_GPU_DIR}/render/textures.cpp
        ${RENUT_GPU_DIR}/render/upload_ring.cpp
        ${RENUT_GPU_DIR}/render/vertex_format.cpp)

    renut_host_shader(${TARGET} overlay_vs vs_6_0)
    renut_host_shader(${TARGET} overlay_ps ps_6_0)
    renut_host_shader(${TARGET} present_vs vs_6_0)
    renut_host_shader(${TARGET} present_ps ps_6_0)
    renut_host_shader(${TARGET} depth_resolve_ps ps_6_0)
    renut_host_shader(${TARGET} depth_resolve_ms_ps ps_6_0)
    renut_host_shader(${TARGET} texture_mip_ps ps_6_0)
    renut_host_shader(${TARGET} rectangle_list_gs gs_6_0)
    renut_host_shader(${TARGET} point_sprite_gs gs_6_0)
    renut_host_shader(${TARGET} stream_convert_cs cs_6_0)
    renut_host_shader(${TARGET} target_readback_cs cs_6_0)
    # renut_anti_aliasing SMAA (backend/smaa.cpp; thirdparty/smaa).
    renut_host_shader(${TARGET} smaa_edge_ps ps_6_0)
    renut_host_shader(${TARGET} smaa_weight_ps ps_6_0)
    renut_host_shader(${TARGET} smaa_blend_ps ps_6_0)

    renut_xenos_shader_cache(${TARGET})

    target_include_directories(${TARGET} PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${RENUT_GPU_DIR}/shaders"
        "${RENUT_GPU_GEN_DIR}"
        "${RENUT_SHADER_COMMON_DIR}")
    # Ahead of the SDK's include directory, which ships a different xxhash.h.
    target_include_directories(${TARGET} BEFORE PRIVATE "${RENUT_XXHASH_DIR}")

    # reNut's overlay (overlay/overlay_ui.cpp) has a Dear ImGui of its own, the same
    # 1.92.5 the SDK exports from rexruntime. ImGui keeps one current context
    # per copy, and the SDK switches its copy's from any thread (the input
    # system asks ImGuiDrawer::GetIO() on the game thread whenever the game
    # polls input), which pulled the overlay's context away mid-frame. Compiled
    # into the executable, these definitions take precedence over rexruntime's
    # imports; the headers go ahead of the SDK's identical ones.
    set(RENUT_IMGUI_DIR "${CMAKE_SOURCE_DIR}/thirdparty/imgui")
    set(renut_imgui_sources
        "${RENUT_IMGUI_DIR}/imgui.cpp"
        "${RENUT_IMGUI_DIR}/imgui_draw.cpp"
        "${RENUT_IMGUI_DIR}/imgui_tables.cpp"
        "${RENUT_IMGUI_DIR}/imgui_widgets.cpp")
    target_sources(${TARGET} PRIVATE ${renut_imgui_sources})
    set_source_files_properties(${renut_imgui_sources} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(${TARGET} BEFORE PRIVATE "${RENUT_IMGUI_DIR}")
    target_compile_definitions(${TARGET} PRIVATE RENUT_RECOMP)
    # rex::o1heap is the installed SDK's export name; a source-tree SDK
    # (REXSDK_DIR) only has the plain target
    if(NOT TARGET rex::o1heap)
        add_library(rex::o1heap ALIAS o1heap)
    endif()
    target_link_libraries(${TARGET} PRIVATE plume renut_zstd renut_smolv renut_xxhash rex::o1heap)

    if(WIN32)
        target_link_libraries(${TARGET} PRIVATE d3d12 dxgi)
    elseif(ANDROID)
        # plume's surface is the ANativeWindow; volk loads libvulkan (or the
        # driver adrenotools picked) itself.
        target_link_libraries(${TARGET} PRIVATE android log)
        # plume's volk loads libvulkan itself: its dlopen is reNut's, where the
        # picked driver stands in (android_driver.cpp). Only plume's: other code
        # (adrenotools) reads dlopen's own machine code.
        target_compile_definitions(plume PRIVATE dlopen=renut_plume_dlopen)
    endif()
    # Linux: volk loads libvulkan itself, and the Wayland or X11 surface is made
    # from SDL's handles (backend/native_window.cpp): no X11 or Wayland library.
endfunction()
