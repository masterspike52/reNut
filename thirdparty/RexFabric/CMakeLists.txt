# RexFabric: mods for rexglue projects. Runtime hooks on every recompiled function, native and Lua mods, the
# game's own modding API, and a mods menu with a Thunderstore browser.
#
#   include(path/to/RexFabric/CMakeLists.txt)
#   rexglue_setup_target(<target>)
#   rexfabric_attach(<target> [WITH_GUIDE] [WITH_TEXTURES] [WITH_NODE_EDITOR])
#
# WITH_GUIDE puts the mods menu in RexLiveless's Guide; attach RexLiveless to the target first. Without it the
# menu is plain ImGui windows. WITH_TEXTURES adds texture packs (rexfabric/textures.h), for a game whose renderer
# looks textures up in them; it isn't available with this SDK. WITH_NODE_EDITOR adds the node editor, which makes Lua mods from node graphs in game.
#
# With RexGlass attached to the same target (before or after), the menus and the node editor are glass.
#
# The project's manifest needs a template_dir under [entrypoint], which RexFabric fills with the codegen
# templates its hook slots need:
#
#   [entrypoint]
#   template_dir = "generated/templates"
#
# The stock templates come from the rexglue-sdk source tree. RexFabric finds them through REXSDK_DIR, or set
# REXFABRIC_SDK_TEMPLATES to <rexglue-sdk>/resources/templates.

include_guard(GLOBAL)

# miniz is C. This file is included, so this runs at the including project's scope, as enable_language must.
enable_language(C)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/rexfabric_templates.cmake)

set(REXFABRIC_SDK_TEMPLATES "" CACHE PATH "The rexglue-sdk's resources/templates folder, matching the SDK the project builds with")

# Lua 5.4.8, compiled as C++ so a Lua error unwinds C++ frames instead of jumping over them.
file(GLOB _rexfabric_lua_sources ${CMAKE_CURRENT_LIST_DIR}/thirdparty/lua/src/*.c)
set_source_files_properties(${_rexfabric_lua_sources} PROPERTIES LANGUAGE CXX)
add_library(rexfabric_lua STATIC ${_rexfabric_lua_sources})
target_include_directories(rexfabric_lua PUBLIC ${CMAKE_CURRENT_LIST_DIR}/thirdparty/lua/src)
target_compile_options(rexfabric_lua PRIVATE -w)
if(UNIX AND NOT APPLE)
  target_compile_definitions(rexfabric_lua PRIVATE LUA_USE_LINUX)
elseif(APPLE)
  target_compile_definitions(rexfabric_lua PRIVATE LUA_USE_MACOSX)
endif()

# miniz 3.0.2, for Thunderstore's zip archives.
add_library(rexfabric_miniz STATIC ${CMAKE_CURRENT_LIST_DIR}/thirdparty/miniz/miniz.c)
target_include_directories(rexfabric_miniz PUBLIC ${CMAKE_CURRENT_LIST_DIR}/thirdparty/miniz)
target_compile_definitions(rexfabric_miniz PUBLIC MINIZ_NO_ZLIB_COMPATIBLE_NAMES=1)
target_compile_options(rexfabric_miniz PRIVATE -w)

# imgui-node-editor (thedmd), the canvas behind the node editor, at the commit Toon pins. It's built once, against
# the SDK's ImGui, the first time a target asks for it. ImGui lives in the runtime library, which exports its functions
# but not its data, so GImGui is read through GetCurrentContext; imgui_internal.h leaves GImGui alone when it's
# defined. Its sources use ImGui's ImVec2 operators, which imgui.h only declares with IMGUI_DEFINE_MATH_OPERATORS.
#
# A game with node graphs of its own links this copy too, rather than building another: two copies in one program
# would be the same symbols twice.
#
#   rexfabric_link_imgui_node_editor(<target>)
function(rexfabric_link_imgui_node_editor target)
  if(NOT TARGET rexfabric_node_editor)
    _rexfabric_node_editor_library()
  endif()
  target_link_libraries(${target} PRIVATE rexfabric_node_editor)
endfunction()

function(_rexfabric_node_editor_library)
  set(dir ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/thirdparty/imgui-node-editor)
  add_library(rexfabric_node_editor STATIC ${dir}/imgui_node_editor.cpp ${dir}/imgui_node_editor_api.cpp ${dir}/imgui_canvas.cpp ${dir}/crude_json.cpp)
  target_include_directories(rexfabric_node_editor PUBLIC ${dir})
  target_link_libraries(rexfabric_node_editor PRIVATE rex::sdk)
  target_compile_definitions(rexfabric_node_editor PRIVATE IMGUI_DEFINE_MATH_OPERATORS "GImGui=ImGui::GetCurrentContext()")
  target_compile_options(rexfabric_node_editor PRIVATE -w)
endfunction()

# The manifest's template_dir, or a fatal error saying how to add it.
function(_rexfabric_template_dir out_var)
  file(GLOB manifests "${CMAKE_SOURCE_DIR}/*_manifest.toml")
  list(LENGTH manifests manifest_count)
  if(NOT manifest_count EQUAL 1)
    message(FATAL_ERROR "RexFabric: expected one *_manifest.toml in ${CMAKE_SOURCE_DIR}, found ${manifest_count}")
  endif()
  file(STRINGS "${manifests}" line REGEX "^[ \t]*template_dir[ \t]*=")
  if(NOT line MATCHES "\"([^\"]+)\"")
    message(FATAL_ERROR "RexFabric: ${manifests} has no template_dir. Add this under [entrypoint]:\n  template_dir = \"generated/templates\"")
  endif()
  set(${out_var} "${CMAKE_SOURCE_DIR}/${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# The SDK's own copies of toml++ (mod.toml) and stb_image (mod icons), which its installed package doesn't carry.
function(_rexfabric_sdk_thirdparty out_var)
  _rexfabric_sdk_templates(templates)
  cmake_path(GET templates PARENT_PATH resources)
  cmake_path(GET resources PARENT_PATH root)
  set(dir "${root}/thirdparty")
  if(NOT EXISTS "${dir}/tomlplusplus/include/toml++/toml.hpp" OR NOT EXISTS "${dir}/stb/stb_image.h")
    message(FATAL_ERROR "RexFabric: can't find toml++ and stb_image in the SDK's thirdparty folder (${dir}).")
  endif()
  set(${out_var} "${dir}" PARENT_SCOPE)
endfunction()

function(_rexfabric_sdk_templates out_var)
  if(REXFABRIC_SDK_TEMPLATES)
    set(dir "${REXFABRIC_SDK_TEMPLATES}")
  elseif(REXSDK_DIR)
    set(dir "${REXSDK_DIR}/resources/templates")
  endif()
  if(NOT dir OR NOT EXISTS "${dir}/codegen")
    message(FATAL_ERROR "RexFabric: can't find the SDK's codegen templates. Set REXFABRIC_SDK_TEMPLATES to <rexglue-sdk>/resources/templates.")
  endif()
  set(${out_var} "${dir}" PARENT_SCOPE)
endfunction()

# Runs after every module is attached, so RexGlass may come before or after RexFabric.
function(_rexfabric_use_glass target)
  get_property(glass TARGET ${target} PROPERTY REXGLASS_ATTACHED)
  if(glass)
    target_compile_definitions(${target} PRIVATE REXFABRIC_WITH_GLASS=1)
  endif()
endfunction()

# Runs once, after every module has added its fragments.
function(_rexfabric_finish)
  _rexfabric_sdk_templates(sdk_dir)
  _rexfabric_template_dir(out_dir)
  rexfabric_write_templates("${sdk_dir}" "${out_dir}")
endfunction()

function(rexfabric_attach target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "WITH_GUIDE;WITH_TEXTURES;WITH_NODE_EDITOR" "" "")
  get_property(attached GLOBAL PROPERTY REXFABRIC_ATTACHED)
  if(attached)
    message(FATAL_ERROR "RexFabric: already attached to ${attached}; a project attaches it once")
  endif()
  set_property(GLOBAL PROPERTY REXFABRIC_ATTACHED ${target})

  set(dir ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
  set(sources
    src/api.cpp src/events.cpp src/game_api.cpp src/guest_hooks.cpp src/hooks.cpp src/host_input.cpp src/library.cpp src/midasm.cpp src/mods.cpp src/native_loader.cpp src/platform.cpp src/rexfabric.cpp src/settings.cpp src/switches.cpp
    src/lua/lua_game.cpp src/lua/lua_host.cpp src/lua/lua_rex.cpp src/lua/lua_timers.cpp src/lua/lua_vm.cpp
    src/net/http.cpp src/net/thunderstore.cpp
    src/ui/image_decode.cpp src/ui/imgui_menu.cpp src/ui/menu_model.cpp src/ui/screen_text.cpp
  )
  if(arg_WITH_GUIDE)
    list(APPEND sources src/ui/guide_menu.cpp)
    target_compile_definitions(${target} PRIVATE REXFABRIC_WITH_GUIDE=1)
  endif()
  if(arg_WITH_TEXTURES)
    message(FATAL_ERROR "RexFabric: WITH_TEXTURES needs the SDK's Xenos texture code and xxHash, which this SDK doesn't offer.")
  endif()
  if(arg_WITH_NODE_EDITOR)
    list(APPEND sources
      src/graph/graph.cpp src/graph/graph_document.cpp src/graph/graph_file.cpp src/graph/lua_compiler.cpp src/graph/node_library.cpp src/graph/nodes_builtin.cpp src/graph/nodes_game.cpp src/graph/nodes_api.cpp
      src/ui/node_editor.cpp src/ui/node_graph_style.cpp src/ui/node_graph_widgets.cpp
    )
    rexfabric_link_imgui_node_editor(${target})
    target_compile_definitions(${target} PRIVATE REXFABRIC_WITH_NODE_EDITOR=1)
  endif()
  list(TRANSFORM sources PREPEND ${dir}/)
  target_sources(${target} PRIVATE ${sources})
  _rexfabric_sdk_thirdparty(sdk_thirdparty)
  target_include_directories(${target} PRIVATE ${dir}/include ${dir}/thirdparty ${sdk_thirdparty}/tomlplusplus/include ${sdk_thirdparty}/stb)
  target_link_libraries(${target} PRIVATE rexfabric_lua rexfabric_miniz)
  # The generated code includes rexfabric/hook_slot.h, and rexglue_setup_target compiles it in its own target.
  foreach(recompiled ${target}_recompiled ${target}_recomp)
    if(TARGET ${recompiled})
      target_include_directories(${recompiled} PRIVATE ${dir}/include)
    endif()
  endforeach()
  if(WIN32)
    target_link_libraries(${target} PRIVATE winhttp shell32)
  else()
    target_link_libraries(${target} PRIVATE ${CMAKE_DL_LIBS})
  endif()

  rexfabric_template_fragment(codegen/pch_h BEFORE "#ifndef DECLARE_REX_FUNC" FILE ${dir}/templates/pch_hooks.inja)
  rexfabric_template_fragment(codegen/init_cpp APPEND FILE ${dir}/templates/init_slots.inja)
  cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL _rexfabric_finish)
  # A deferred call reads its arguments when it runs, so the target's name is put in now.
  cmake_language(EVAL CODE "cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]] CALL _rexfabric_use_glass [[${target}]])")
endfunction()
