# Writes engine_version.h from its template, at build time rather than at
# configure time, so the recorded source revision belongs to the commit
# being built. A configure-time revision goes stale the moment anyone
# commits without re-running CMake, and a build id naming the wrong commit
# is worse than none: a crash report would point at code that was never in
# the binary.
#
# configure_file rewrites the output only when its content changed, so
# rebuilding without moving HEAD does not invalidate everything that
# includes the header.
#
# Inputs: TEMPLATE, OUTPUT, SOURCE_DIR (the repository to describe), and
# the identity values the caller already knows -- PROJECT_VERSION_MAJOR /
# _MINOR / _PATCH, PROJECT_VERSION, ENGINE_BUILD_COMPILER,
# ENGINE_BUILD_TYPE, ENGINE_BUILD_PLATFORM, ENGINE_BUILD_FLOAT.

foreach(var TEMPLATE OUTPUT SOURCE_DIR PROJECT_VERSION)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} is required")
    endif()
endforeach()

# "unknown" rather than an empty string or a silent omission: a build from
# a source drop with no git history is a legitimate case, and the crash
# report has to say which case it is instead of leaving the field blank.
set(ENGINE_BUILD_DESCRIBE "unknown")

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --tags
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE git_describe
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE git_result
        ERROR_QUIET
    )
    if((git_result EQUAL 0) AND git_describe)
        # The identity goes into a C string literal, so anything that could
        # end it early or escape it is refused rather than emitted.
        if(git_describe MATCHES "^[A-Za-z0-9._/+-]+$")
            set(ENGINE_BUILD_DESCRIBE "${git_describe}")
        else()
            message(WARNING
                "git describe returned unusable characters; recording "
                "the revision as unknown: ${git_describe}")
        endif()
    endif()
endif()

configure_file("${TEMPLATE}" "${OUTPUT}" @ONLY)
