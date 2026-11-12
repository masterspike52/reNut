# RexGlass: glass for ImGui in rexglue projects, after the Toon engine's Liquid Glass UI. Glass windows and panels,
# drawn as ordinary ImGui geometry so they work on any renderer, showing the game through them when the game's
# renderer hands over its blurred frame.
#
#   include(path/to/RexGlass/CMakeLists.txt)
#   rexglue_setup_target(<target>)
#   rexglass_attach(<target>)
#
# Other modules use it when it's there: they check the REXGLASS_ATTACHED target property, in either order of
# attaching, and include <rexglass/...> from the include folder RexGlass adds to the target.

include_guard(GLOBAL)

function(rexglass_attach target)
  set(dir ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
  set(sources ${dir}/src/aurora.cpp ${dir}/src/panel.cpp ${dir}/src/rexglass.cpp ${dir}/src/theme.cpp ${dir}/src/widgets.cpp ${dir}/src/windows.cpp)
  target_sources(${target} PRIVATE ${sources})
  target_include_directories(${target} PRIVATE ${dir}/include)
  # ImGui lives in the SDK's runtime library, which exports its functions but not its data, so RexGlass's use of
  # imgui_internal.h reads GImGui through GetCurrentContext. imgui_internal.h leaves GImGui alone when it's defined.
  set_source_files_properties(${sources} TARGET_DIRECTORY ${target} PROPERTIES COMPILE_DEFINITIONS "GImGui=ImGui::GetCurrentContext()")
  set_property(TARGET ${target} PROPERTY REXGLASS_ATTACHED TRUE)
endfunction()
