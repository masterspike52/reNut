# NutTool's core (formats, editing, the reNut makers), built into reNut for the in-game NutTool.
# Include after RexFabric: it uses RexFabric's Lua and the SDK's stb_image.

set(_nuttool_dir "${CMAKE_CURRENT_LIST_DIR}")
set(_nuttool_core
    common json deflate image lzx xctd caff streambundle assetdb container texture loctext font gamescript
    media model animkeys modelimport nbpatch objparams renutpart renutmodel renutvehicle renutmake renutchars
    actorswap renutworld renutnpc gltfobj collada blockmodel npcmodel scriptgraph scriptgraph_nodes ops xui
    commands script fbx audio rigedit)
list(TRANSFORM _nuttool_core PREPEND "${_nuttool_dir}/core/")
list(TRANSFORM _nuttool_core APPEND ".cpp")

add_library(nutcore STATIC ${_nuttool_core})
target_include_directories(nutcore PUBLIC "${_nuttool_dir}/core")
target_compile_features(nutcore PUBLIC cxx_std_20)
target_compile_definitions(nutcore PRIVATE _CRT_SECURE_NO_WARNINGS NUTTOOL_LUA=1 NUTTOOL_STB=1)
if(MSVC)
    target_compile_options(nutcore PRIVATE /utf-8 /EHsc)
endif()
target_link_libraries(nutcore PUBLIC rexfabric_lua)
_rexfabric_sdk_thirdparty(_nuttool_sdk_thirdparty)
target_include_directories(nutcore PRIVATE "${_nuttool_sdk_thirdparty}/stb")

# FFmpeg decodes the XMA audio; without it only the PCM waves play.
option(RENUT_NUTTOOL_FFMPEG "Link FFmpeg into NutTool to play the game's XMA audio" ON)
if(RENUT_NUTTOOL_FFMPEG)
    find_path(RENUT_FFMPEG_INCLUDE libavcodec/avcodec.h)
    find_library(RENUT_AVFORMAT avformat)
    find_library(RENUT_AVCODEC avcodec)
    find_library(RENUT_SWRESAMPLE swresample)
    find_library(RENUT_AVUTIL avutil)
endif()
if(RENUT_NUTTOOL_FFMPEG AND RENUT_FFMPEG_INCLUDE AND RENUT_AVFORMAT AND RENUT_AVCODEC AND RENUT_SWRESAMPLE AND RENUT_AVUTIL)
    target_include_directories(nutcore PRIVATE "${RENUT_FFMPEG_INCLUDE}")
    target_link_libraries(nutcore PUBLIC "${RENUT_AVFORMAT}" "${RENUT_AVCODEC}" "${RENUT_SWRESAMPLE}" "${RENUT_AVUTIL}")
    if(WIN32)
        target_link_libraries(nutcore PUBLIC mfuuid ole32 strmiids user32 secur32 ncrypt crypt32 ws2_32 bcrypt)
    endif()
    target_compile_definitions(nutcore PRIVATE NUTTOOL_FFMPEG=1)
    message(STATUS "NutTool: FFmpeg found, XMA audio plays")
else()
    message(STATUS "NutTool: no FFmpeg, only PCM audio plays")
endif()

# The in-game NutTool's windows (NutTool's GUI on reNut's overlay); the video player decodes with Media Foundation.
set(_nuttool_ui
    app catalog doc_caff doc_text doc_font doc_misc doc_script doc_texture texture_edit doc_model studio_view rig_edit
    script_window tools doc_audio audio_view video_player)
list(TRANSFORM _nuttool_ui PREPEND "${_nuttool_dir}/ui/")
list(TRANSFORM _nuttool_ui APPEND ".cpp")
if(WIN32)
    list(APPEND _nuttool_ui "${_nuttool_dir}/ui/video_player_mf.cpp")
else()
    list(APPEND _nuttool_ui "${_nuttool_dir}/ui/video_player_none.cpp")
endif()

# Adds the in-game NutTool (its windows, studio and saving) to reNut's target.
function(renut_nuttool_setup target)
    target_sources(${target} PRIVATE ${_nuttool_ui} "${_nuttool_dir}/studio.cpp" "${_nuttool_dir}/frontend.cpp"
                                     "${_nuttool_dir}/session.cpp" "${_nuttool_dir}/workspace.cpp")
    target_include_directories(${target} PRIVATE "${_nuttool_dir}/ui")
    target_link_libraries(${target} PRIVATE nutcore)
    if(WIN32)
        target_link_libraries(${target} PRIVATE mfplat mfreadwrite mfuuid shlwapi ole32)
    endif()
    if(MSVC)
        set_source_files_properties(${_nuttool_ui} PROPERTIES COMPILE_OPTIONS "/utf-8")
    endif()
endfunction()
