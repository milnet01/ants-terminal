# ANTS-1394 — build-time regeneration of build_info.h.
#
# Invoked by the ants_build_info custom target on every build (NOT
# every configure). Re-evaluates ANTS_BUILD_DATE / ANTS_BUILD_TIME /
# ANTS_BUILD_COMMIT against the current wall-clock + git state, then
# rewrites build_info.h via configure_file (which uses
# copy_if_different — content-stable rebuilds don't touch the mtime,
# so ccache stays warm on TUs that include the header).
#
# Required cache vars (all passed via -D on the cmake -P command):
#   SOURCE_DIR        — repo root (for git rev-parse + template path)
#   OUTPUT_FILE       — destination path for build_info.h
#   TEMPLATE_FILE     — path to cmake/build_info.h.in
#   ANTS_BUILD_TYPE   — CMAKE_BUILD_TYPE (Release/Debug/...)

if(NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_FILE
   OR NOT DEFINED TEMPLATE_FILE OR NOT DEFINED ANTS_BUILD_TYPE)
    message(FATAL_ERROR
        "GenerateBuildInfo.cmake: missing required -D vars "
        "(SOURCE_DIR, OUTPUT_FILE, TEMPLATE_FILE, ANTS_BUILD_TYPE)")
endif()

# Honor SOURCE_DATE_EPOCH for reproducible builds (distro packagers).
if(DEFINED ENV{SOURCE_DATE_EPOCH})
    string(TIMESTAMP ANTS_BUILD_DATE "%Y-%m-%d" UTC)
    string(TIMESTAMP ANTS_BUILD_TIME "%H:%M" UTC)
else()
    string(TIMESTAMP ANTS_BUILD_DATE "%Y-%m-%d")
    string(TIMESTAMP ANTS_BUILD_TIME "%H:%M")
endif()

if(NOT ANTS_BUILD_TYPE)
    set(ANTS_BUILD_TYPE "Unknown")
endif()

set(ANTS_BUILD_COMMIT "unknown")
find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE AND EXISTS "${SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE _git_sha_raw
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _git_sha_result
        ERROR_QUIET)
    if(_git_sha_result EQUAL 0 AND _git_sha_raw)
        set(ANTS_BUILD_COMMIT "${_git_sha_raw}")
    endif()
endif()

# copy_if_different semantics — only touches the file when content
# actually changed, so an intra-minute rebuild doesn't invalidate
# the ccache entry for aboutdialogs.cpp / mainwindow.cpp.
configure_file("${TEMPLATE_FILE}" "${OUTPUT_FILE}" @ONLY)
