# SentryWork - Sentry crash reports for ReXGlue recomps
#
#   include(thirdparty/sentrywork/CMakeLists.txt)
#   sentrywork_attach(<target> DSN "<your Sentry project's DSN>"   # after rexglue_setup_target(<target>)
#                     [RELEASE "<name>@<version>"] [ENVIRONMENT <name>]
#                     [ORG <org slug> PROJECT <project slug>])     # adds <target>_upload_symbols
#
# and in the app (include/sentrywork/sentrywork.h): sentrywork::Init from
# OnPostInitLogging, sentrywork::OnRuntimeReady from OnPostSetup and
# sentrywork::Shutdown from OnShutdown.
#
# The Sentry Native SDK (sentry-native/, 0.17.3, MIT) builds as a static
# library with its "native" backend: a crash is written up and sent by the
# sentry-crash helper process, copied next to the executable here. (Crashpad,
# Sentry's default, needs MSVC's compiler driver on Windows; ReXGlue projects
# build with clang++, so sentry-native/ is vendored without it.) On Linux it
# sends over libcurl: install
# libcurl4-openssl-dev to build.
#
# SentryWork's own sources compile straight into the game executable, as the
# Rexglue Guide's do: they define cvars and use the SDK's headers like the
# game's own code.

set(SENTRYWORK_BACKEND "native" CACHE STRING
  "sentry-native crash backend: native (ships sentry-crash next to the exe), breakpad or inproc")
set_property(CACHE SENTRYWORK_BACKEND PROPERTY STRINGS native breakpad inproc)
# For forks and local builds that should not report to the owner's project.
option(SENTRYWORK_DISABLE "Build without crash reporting (no DSN is compiled in)" OFF)
# Sentry names a crash's functions from the build's debug files, so every
# configuration gets line tables (function names and lines, not full debug
# info) and, on Windows, a .pdb next to the executable. Ship the executable,
# keep the .pdb, and upload both with the <target>_upload_symbols target.
option(SENTRYWORK_DEBUG_INFO "Build the game with line tables (and a .pdb) for Sentry's stack traces" ON)
# npm's install puts a shell script named sentry-cli beside sentry-cli.cmd,
# and Windows runs only the latter.
if(CMAKE_HOST_WIN32)
  set(sentry_cli_names sentry-cli.exe sentry-cli.cmd sentry-cli)
else()
  set(sentry_cli_names sentry-cli)
endif()
find_program(SENTRYWORK_SENTRY_CLI NAMES ${sentry_cli_names}
             DOC "sentry-cli, for <target>_upload_symbols")
# WSL appends the Windows PATH, where npm's sentry-cli shell script needs a
# Windows node; a Linux build only takes a Linux sentry-cli.
if(NOT CMAKE_HOST_WIN32 AND SENTRYWORK_SENTRY_CLI MATCHES "^/mnt/[A-Za-z]/")
  unset(SENTRYWORK_SENTRY_CLI CACHE)
  find_program(SENTRYWORK_SENTRY_CLI NAMES sentry-cli NO_SYSTEM_ENVIRONMENT_PATH
               PATHS /usr/local/bin /usr/bin "$ENV{HOME}/.local/bin"
               DOC "sentry-cli, for <target>_upload_symbols")
endif()

function(sentrywork_attach target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "DSN;RELEASE;ENVIRONMENT;ORG;PROJECT" "")
  set(dir ${CMAKE_CURRENT_FUNCTION_LIST_DIR})

  if(SENTRYWORK_DISABLE)
    set(arg_DSN "")
  elseif(NOT arg_DSN)
    message(WARNING "sentrywork_attach(${target}): no DSN given, so crash reports are off")
  endif()
  if(NOT arg_RELEASE)
    if(PROJECT_VERSION)
      set(arg_RELEASE "${target}@${PROJECT_VERSION}")
    else()
      set(arg_RELEASE "${target}@dev")
    endif()
  endif()
  if(NOT arg_ENVIRONMENT)
    set(arg_ENVIRONMENT "$<IF:$<CONFIG:Debug>,development,production>")
  endif()

  if(NOT SENTRYWORK_BACKEND MATCHES "^(native|breakpad|inproc)$")
    message(FATAL_ERROR "SENTRYWORK_BACKEND must be native, breakpad or inproc "
                        "(crashpad is not vendored), not '${SENTRYWORK_BACKEND}'")
  endif()
  if(NOT TARGET sentry)
    # option() and the SDK's guarded cache entries take these as given.
    set(SENTRY_BACKEND ${SENTRYWORK_BACKEND})
    set(SENTRY_BUILD_SHARED_LIBS OFF)
    set(SENTRY_BUILD_TESTS OFF)
    set(SENTRY_BUILD_EXAMPLES OFF)
    set(SENTRY_ENABLE_INSTALL OFF)
    add_subdirectory(${dir}/sentry-native ${CMAKE_BINARY_DIR}/sentrywork EXCLUDE_FROM_ALL)
  endif()

  set(sources ${dir}/src/sentrywork.cpp)
  # Linux: crashes the runtime's signal handler would swallow (see there).
  if(NOT WIN32)
    list(APPEND sources ${dir}/src/fault_fallback_posix.cpp)
  endif()
  target_sources(${target} PRIVATE ${sources})
  set(defines
    "SENTRYWORK_DSN=\"${arg_DSN}\""
    "SENTRYWORK_RELEASE=\"${arg_RELEASE}\""
    "SENTRYWORK_ENVIRONMENT=\"${arg_ENVIRONMENT}\"")
  # getenv, which MSVC's CRT marks deprecated (Windows only).
  if(WIN32)
    list(APPEND defines _CRT_SECURE_NO_WARNINGS)
  endif()
  set_source_files_properties(${sources} PROPERTIES COMPILE_DEFINITIONS "${defines}")
  target_include_directories(${target} PRIVATE ${dir}/include)
  target_link_libraries(${target} PRIVATE sentry)

  # The native backend's crash daemon, which the game starts at Init.
  if(TARGET sentry-crash)
    add_dependencies(${target} sentry-crash)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:sentry-crash> $<TARGET_FILE_DIR:${target}>
      VERBATIM)
  endif()

  # Generated recomp code already has line tables (REXGLUE_RECOMP_DEBUG_INFO);
  # this adds them to the game's own sources and links them into a .pdb.
  # /DEBUG turns off unused-code removal by default, so it goes back on.
  if(SENTRYWORK_DEBUG_INFO)
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

  # cmake --build <dir> --target <target>_upload_symbols, with SENTRY_AUTH_TOKEN
  # set: sends this build's executable, .pdb and the rexglue runtime to the
  # Sentry project, before the build goes out to players.
  if(arg_ORG AND arg_PROJECT)
    set(files "$<TARGET_FILE:${target}>")
    if(WIN32)
      list(APPEND files "$<TARGET_FILE_DIR:${target}>/$<TARGET_FILE_BASE_NAME:${target}>.pdb")
    endif()
    list(APPEND files "$<TARGET_FILE:rex::runtime>")
    list(JOIN files "|" files)
    add_custom_target(${target}_upload_symbols
      COMMAND ${CMAKE_COMMAND} "-DSENTRY_CLI=${SENTRYWORK_SENTRY_CLI}"
              "-DORG=${arg_ORG}" "-DPROJECT=${arg_PROJECT}" "-DFILES=${files}"
              -P ${dir}/cmake/upload_symbols.cmake
      DEPENDS ${target}
      COMMENT "Uploading ${target}'s debug files to Sentry (${arg_ORG}/${arg_PROJECT})"
      USES_TERMINAL VERBATIM)
  endif()
endfunction()
