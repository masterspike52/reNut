# Linux only (included from CMakeLists.txt when building for Linux).
#
# Packs renut into <build dir>/renut-x86_64.AppImage after every build, with
# linuxdeploy: the executable, the shared libraries it links that a Linux
# system cannot be expected to have (the rexglue runtime and what it pulls
# in), the desktop entry and the icon. linuxdeploy's exclude list keeps the
# system's own C, graphics and audio libraries out: the Vulkan driver is the
# user's. The shader cache is compiled into the executable, and the game data
# is not packed: the path setup asks for it on first run. Settings, logs and
# saves go to ~/.local/share/renut (src/renut_engine/app/app_paths.h), as the
# AppImage itself is mounted read-only.
#
# RENUT_APPIMAGE=OFF skips the step; RENUT_LINUXDEPLOY names a linuxdeploy to
# use, else one on PATH, else it is downloaded into the build directory once.

option(RENUT_APPIMAGE "Pack renut into an AppImage after building (Linux)" ON)
set(RENUT_LINUXDEPLOY "" CACHE FILEPATH
    "linuxdeploy executable (default: from PATH, else downloaded into the build directory)")

if(NOT RENUT_APPIMAGE)
    return()
endif()

set(renut_linuxdeploy "${RENUT_LINUXDEPLOY}")
if(NOT renut_linuxdeploy)
    find_program(renut_linuxdeploy_on_path NAMES linuxdeploy linuxdeploy-x86_64.AppImage)
    set(renut_linuxdeploy "${renut_linuxdeploy_on_path}")
endif()
set(renut_fetch_linuxdeploy "")
if(NOT renut_linuxdeploy)
    set(renut_linuxdeploy "${CMAKE_BINARY_DIR}/tools/linuxdeploy-x86_64.AppImage")
    set(renut_fetch_linuxdeploy
        COMMAND "${CMAKE_COMMAND}" -DDEST=${renut_linuxdeploy}
                -P "${CMAKE_SOURCE_DIR}/cmake/fetch_linuxdeploy.cmake")
endif()

set(renut_appdir "${CMAKE_BINARY_DIR}/AppDir")

# SentryWork's crash daemon goes next to renut, with the libraries it needs.
set(renut_sentry_crash "")
if(TARGET sentry-crash)
    set(renut_sentry_crash
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:sentry-crash>" "${renut_appdir}/usr/bin/sentry-crash")
    set(renut_sentry_crash_executable --executable "${renut_appdir}/usr/bin/sentry-crash")
endif()
set(renut_appimage "${CMAKE_BINARY_DIR}/renut-x86_64.AppImage")

add_custom_command(
    OUTPUT "${renut_appimage}"
    ${renut_fetch_linuxdeploy}
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${renut_appdir}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${renut_appdir}/usr/bin"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:renut>" "${renut_appdir}/usr/bin/renut"
    ${renut_sentry_crash}
    # The rexglue runtime is found where the SDK installed it, and the EOS SDK
    # next to renut (copied there after linking). linuxdeploy runs extracted,
    # so FUSE is not needed on the build machine.
    COMMAND "${CMAKE_COMMAND}" -E env
            "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:rex::runtime>:$<TARGET_FILE_DIR:renut>"
            APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64
            "LDAI_OUTPUT=${renut_appimage}" "OUTPUT=${renut_appimage}"
            "${renut_linuxdeploy}" --appdir "${renut_appdir}"
            --executable "${renut_appdir}/usr/bin/renut"
            ${renut_sentry_crash_executable}
            --desktop-file "${CMAKE_SOURCE_DIR}/icon/renut.desktop"
            --icon-file "${CMAKE_SOURCE_DIR}/icon/renut.png"
            --output appimage
    DEPENDS renut "${CMAKE_SOURCE_DIR}/icon/renut.desktop" "${CMAKE_SOURCE_DIR}/icon/renut.png"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Packing renut into ${renut_appimage}"
    USES_TERMINAL VERBATIM)
add_custom_target(renut_appimage ALL DEPENDS "${renut_appimage}")
