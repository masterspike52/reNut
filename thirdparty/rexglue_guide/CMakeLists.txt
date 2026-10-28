# Rexglue Guide
#
#   include(thirdparty/rexglue_guide/CMakeLists.txt)
#   rexglue_guide_attach(<target>)   # after rexglue_setup_target(<target>)
#
# The sources compile straight into the game executable rather than a static
# library: the Guide defines XAM import functions, and only object files are
# sure to win over the rexglue runtime DLL's versions at link time.

function(rexglue_guide_attach target)
  set(dir ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
  set(sources
    ${dir}/src/assets.cpp
    ${dir}/src/audio.cpp
    ${dir}/src/file_picker.cpp
    ${dir}/src/guest.cpp
    ${dir}/src/guide.cpp
    ${dir}/src/guide_account.cpp
    ${dir}/src/guide_draw.cpp
    ${dir}/src/guide_draw_list.cpp
    ${dir}/src/guide_friends.cpp
    ${dir}/src/guide_games.cpp
    ${dir}/src/guide_input.cpp
    ${dir}/src/guide_messages.cpp
    ${dir}/src/guide_pages.cpp
    ${dir}/src/guide_popups.cpp
    ${dir}/src/guide_preferences.cpp
    ${dir}/src/guide_themes.cpp
    ${dir}/src/http.cpp
    ${dir}/src/input.cpp
    ${dir}/src/keyboard.cpp
    ${dir}/src/live_sessions.cpp
    ${dir}/src/live_stats.cpp
    ${dir}/src/mock_backend.cpp
    ${dir}/src/net.cpp
    ${dir}/src/photo.cpp
    ${dir}/src/server_backend.cpp
    ${dir}/src/theme_background.cpp
    ${dir}/src/themes.cpp
    ${dir}/src/ui.cpp
    ${dir}/src/xam_live.cpp
  )
  target_sources(${target} PRIVATE ${sources})
  # The Guide reads its settings with getenv, which MSVC's CRT marks deprecated
  # in favour of _dupenv_s (Windows only).
  if(WIN32)
    set_source_files_properties(${sources} PROPERTIES COMPILE_DEFINITIONS _CRT_SECURE_NO_WARNINGS)
  else()
    # HTTPS loads the system's libcurl with dlopen (src/http.cpp).
    target_link_libraries(${target} PRIVATE ${CMAKE_DL_LIBS})
  endif()
  target_include_directories(${target} PRIVATE ${dir}/include ${dir}/third_party)
  # The photo picker's Open dialog (file_picker.cpp).
  if(WIN32)
    target_link_libraries(${target} PRIVATE comdlg32 ole32)
  endif()
endfunction()
