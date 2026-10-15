# Writes the version this build comes from to a C++ header, the way TVTest's
# TVTestVersionHashGen.bat does: it runs at every build (a custom target), the
# header lives in the build directory, and the sources include it with
# __has_include.
#
# The version is `git describe --tags --always --dirty`, e.g. "v20261015a" for
# a tagged commit, "v20261015a-2-gb96f5d2" two commits after it, with "-dirty"
# when there are uncommitted changes, and "unknown" without git. The header is
# rewritten only when it changes, so it does not cause rebuilds.
#
#   cmake -DSOURCE_DIR=<repository> -DOUTPUT=<header> -DNAME=<macro prefix>
#         -P GenerateVersionHeader.cmake
#
#   #define <NAME>_VERSION_STRING  "v20261015a-2-gb96f5d2"
#   #define <NAME>_VERSION_NUMBERS 2026,10,15,2   (0,0,0,0 without a vYYYYMMDD tag)

set(version "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" describe --tags --always
        OUTPUT_VARIABLE described
        RESULT_VARIABLE result
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(result EQUAL 0 AND described)
        set(version "${described}")
        # "-dirty" like `git describe --dirty`, but changes inside submodules
        # do not count: building TSDuck (thirdparty/dantto4k/thirdparty/tsduck)
        # patches its MSVC settings. A submodule moved to another commit still
        # counts.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" status --porcelain --untracked-files=no --ignore-submodules=dirty
            OUTPUT_VARIABLE changes
            RESULT_VARIABLE result
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(NOT result EQUAL 0 OR changes)
            string(APPEND version "-dirty")
        endif()
    endif()
endif()

set(numbers "0,0,0,0")
if(version MATCHES "^v([0-9][0-9][0-9][0-9])([0-9][0-9])([0-9][0-9])[a-z]?(-([0-9]+)-g)?")
    math(EXPR year "${CMAKE_MATCH_1}")
    math(EXPR month "${CMAKE_MATCH_2}")
    math(EXPR day "${CMAKE_MATCH_3}")
    set(commits 0)
    if(CMAKE_MATCH_5)
        set(commits "${CMAKE_MATCH_5}")
        if(commits GREATER 65535)
            set(commits 65535)
        endif()
    endif()
    set(numbers "${year},${month},${day},${commits}")
endif()

set(content "#define ${NAME}_VERSION_STRING \"${version}\"\n#define ${NAME}_VERSION_NUMBERS ${numbers}\n")
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" existing)
    if(existing STREQUAL content)
        return()
    endif()
endif()
file(WRITE "${OUTPUT}" "${content}")
message(STATUS "${NAME} version: ${version}")
