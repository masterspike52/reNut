# cmake -DCAPTURE_DIR=<dir> -DDUMP_DIR=<dir> -P collect_captured_shaders.cmake
# Copies the shader containers reNut captured at runtime (shaders its cache
# lacked: the game compiles a few itself, which no dump of its files has)
# into the shader dump directory, each one it does not have yet, so
# XenosRecomp recompiles them with the rest.
file(MAKE_DIRECTORY "${DUMP_DIR}")
file(GLOB captured "${CAPTURE_DIR}/*.bin")
foreach(source IN LISTS captured)
    get_filename_component(name "${source}" NAME)
    if(NOT EXISTS "${DUMP_DIR}/${name}")
        file(COPY "${source}" DESTINATION "${DUMP_DIR}")
        message(STATUS "Added captured shader ${name} to ${DUMP_DIR}")
    endif()
endforeach()
