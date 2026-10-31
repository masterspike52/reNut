# Run by the <target>_upload_symbols target (see sentrywork_attach): sends the
# build's debug files to Sentry, so its crash reports show function names.
#
#   -DSENTRY_CLI=<sentry-cli> -DORG=<org> -DPROJECT=<project> -DFILES=<a|b|...>

if(NOT SENTRY_CLI)
  message(FATAL_ERROR
    "sentry-cli was not found. Install it (npm install -g @sentry/cli) and reconfigure,\n"
    "or pass -DSENTRYWORK_SENTRY_CLI=<path to sentry-cli>.")
endif()
if("$ENV{SENTRY_AUTH_TOKEN}" STREQUAL "")
  message(FATAL_ERROR
    "SENTRY_AUTH_TOKEN is not set. Create an organization auth token in Sentry\n"
    "(Settings > Developer Settings > Organization Tokens) and set it in this shell.")
endif()

string(REPLACE "|" ";" files "${FILES}")
set(found "")
foreach(file IN LISTS files)
  if(EXISTS "${file}")
    list(APPEND found "${file}")
  else()
    message(STATUS "Not uploading ${file}: it does not exist")
  endif()
endforeach()

execute_process(
  COMMAND "${SENTRY_CLI}" debug-files upload --org "${ORG}" --project "${PROJECT}" --wait ${found}
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "sentry-cli could not upload the debug files (exit code ${result})")
endif()
