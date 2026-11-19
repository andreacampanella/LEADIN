cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The version string carries the board, so an uploaded image for another
# board is refused (ota.c compares the part after the dash).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS main/leadin.h)   # re-read on a version change
file(STRINGS main/leadin.h _ver REGEX "LEADIN_VERSION")
string(REGEX REPLACE ".*\"([^\"]*)\".*" "\\1" _ver "${_ver}")
set(_board "unknown")
foreach(_f ${SDKCONFIG_DEFAULTS})        # "...;boards/<name>/sdkconfig.board"
    if(_f MATCHES "boards/([^/]+)/sdkconfig.board")
        set(_board "${CMAKE_MATCH_1}")
    endif()
endforeach()
set(PROJECT_VER "${_ver}-${_board}")
project(leadin)
