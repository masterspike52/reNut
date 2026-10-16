# cmake -DDEST=<path> -P fetch_linuxdeploy.cmake
# Downloads linuxdeploy (continuous release, x86_64 AppImage) to DEST unless
# it is already there, and makes it executable. Its AppImage output plugin is
# built in.
if(EXISTS "${DEST}")
    return()
endif()
set(url "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage")
message(STATUS "Downloading linuxdeploy from ${url}")
file(DOWNLOAD "${url}" "${DEST}.part" STATUS status TLS_VERIFY ON)
list(GET status 0 code)
if(NOT code EQUAL 0)
    file(REMOVE "${DEST}.part")
    list(GET status 1 reason)
    message(FATAL_ERROR "linuxdeploy download failed (${reason}); install linuxdeploy, or set "
                        "RENUT_LINUXDEPLOY to it")
endif()
file(RENAME "${DEST}.part" "${DEST}")
file(CHMOD "${DEST}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                                 WORLD_READ WORLD_EXECUTE)
