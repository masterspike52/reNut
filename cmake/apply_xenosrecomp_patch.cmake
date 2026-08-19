# Idempotent patch apply for the XenosRecomp FetchContent step (see
# rexglue_xenosrecomp.cmake). A second configure (already-patched source dir,
# FetchContent skips re-cloning) must not fail -- `git apply --check`
# returning non-zero means the patch is already applied, so skip re-applying.
#
# Written as a portable CMake script rather than `sh -c "... && ... || true"`:
# the shell-based version only worked on Linux/macOS, since `sh` is not on
# PATH by default on Windows -- broke the whole FetchContent populate step
# there (patch never applies, build fails at the populate subbuild instead).
# execute_process() needs no shell at all, so this works identically
# everywhere CMake itself runs.

find_program(RENUT_GIT NAMES git)
if(NOT RENUT_GIT)
    message(FATAL_ERROR "git not found - cannot apply ${PATCH_FILE}")
endif()

execute_process(
    COMMAND "${RENUT_GIT}" apply --check "${PATCH_FILE}"
    RESULT_VARIABLE check_result
    OUTPUT_QUIET
    ERROR_QUIET
)

if(check_result EQUAL 0)
    execute_process(
        COMMAND "${RENUT_GIT}" apply "${PATCH_FILE}"
        RESULT_VARIABLE apply_result
    )
    if(NOT apply_result EQUAL 0)
        message(FATAL_ERROR "Failed to apply ${PATCH_FILE}")
    endif()
    message(STATUS "Applied ${PATCH_FILE}")
endif()
