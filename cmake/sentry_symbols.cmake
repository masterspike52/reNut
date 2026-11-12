# Debug files for Sentry's stack traces (RexCrash sends the reports; this keeps
# what it needs to name a crash's functions).
#
#   include(cmake/sentry_symbols.cmake)
#   renut_sentry_symbols(<target> ORG <org slug> PROJECT <project slug>)   # adds <target>_upload_symbols
#
# Every configuration gets line tables (function names and lines, not full
# debug info) and, on Windows, a .pdb next to the executable. Ship the
# executable, keep the .pdb, and upload both with
#   cmake --build <dir> --target <target>_upload_symbols
# with SENTRY_AUTH_TOKEN set, before the build goes out to players. The
# recompiled code already has line tables (REXGLUE_RECOMP_DEBUG_INFO).

option(RENUT_DEBUG_INFO "Build the game with line tables (and a .pdb) for Sentry's stack traces" ON)

# npm's install puts a shell script named sentry-cli beside sentry-cli.cmd,
# and Windows runs only the latter.
if(CMAKE_HOST_WIN32)
  set(renut_sentry_cli_names sentry-cli.exe sentry-cli.cmd sentry-cli)
else()
  set(renut_sentry_cli_names sentry-cli)
endif()
find_program(RENUT_SENTRY_CLI NAMES ${renut_sentry_cli_names}
             DOC "sentry-cli, for <target>_upload_symbols")
# WSL appends the Windows PATH, where npm's sentry-cli shell script needs a
# Windows node; a Linux build only takes a Linux sentry-cli.
if(NOT CMAKE_HOST_WIN32 AND RENUT_SENTRY_CLI MATCHES "^/mnt/[A-Za-z]/")
  unset(RENUT_SENTRY_CLI CACHE)
  find_program(RENUT_SENTRY_CLI NAMES sentry-cli NO_SYSTEM_ENVIRONMENT_PATH
               PATHS /usr/local/bin /usr/bin "$ENV{HOME}/.local/bin"
               DOC "sentry-cli, for <target>_upload_symbols")
endif()

function(renut_sentry_symbols target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "ORG;PROJECT" "")

  # /DEBUG turns off unused-code removal by default, so it goes back on.
  if(RENUT_DEBUG_INFO)
    set(no_debug_config "$<NOT:$<CONFIG:Debug,RelWithDebInfo>>")
    if(MSVC)
      target_compile_options(${target} PRIVATE "$<${no_debug_config}:/Z7>")
      target_link_options(${target} PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      target_compile_options(${target} PRIVATE "$<${no_debug_config}:-gline-tables-only>")
      if(WIN32)
        target_link_options(${target} PRIVATE -g LINKER:/OPT:REF LINKER:/OPT:ICF)
      endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      target_compile_options(${target} PRIVATE "$<${no_debug_config}:-g1>")
    endif()
  endif()

  if(arg_ORG AND arg_PROJECT)
    set(files "$<TARGET_FILE:${target}>")
    if(WIN32)
      list(APPEND files "$<TARGET_FILE_DIR:${target}>/$<TARGET_FILE_BASE_NAME:${target}>.pdb")
    endif()
    foreach(library rex::base rex::ppc rex::kernel rex::xam rex::app rex::xbdm)
      list(APPEND files "$<TARGET_FILE:${library}>")
    endforeach()
    list(JOIN files "|" files)
    add_custom_target(${target}_upload_symbols
      COMMAND ${CMAKE_COMMAND} "-DSENTRY_CLI=${RENUT_SENTRY_CLI}"
              "-DORG=${arg_ORG}" "-DPROJECT=${arg_PROJECT}" "-DFILES=${files}"
              -P ${CMAKE_SOURCE_DIR}/cmake/upload_symbols.cmake
      DEPENDS ${target}
      COMMENT "Uploading ${target}'s debug files to Sentry (${arg_ORG}/${arg_PROJECT})"
      USES_TERMINAL VERBATIM)
  endif()
endfunction()
