cmake_minimum_required(VERSION 3.21)

# name-version.txt is the single source of truth: line 1 is the product name, line 2 the version (MAJOR.MINOR.PATCH).
# CMake turns it into the generated header and version resources, the package file name, and the GitHub release.
set(FGC_NAME_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/name-version.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FGC_NAME_VERSION_FILE}")
file(STRINGS "${FGC_NAME_VERSION_FILE}" FGC_NAME_VERSION_LINES)
list(LENGTH FGC_NAME_VERSION_LINES FGC_NAME_VERSION_LINE_COUNT)
if(FGC_NAME_VERSION_LINE_COUNT LESS 2)
    message(FATAL_ERROR "name-version.txt must contain the project name on line 1 and the version on line 2")
endif()
list(GET FGC_NAME_VERSION_LINES 0 FGC_PROJECT_NAME)
list(GET FGC_NAME_VERSION_LINES 1 FGC_PROJECT_VERSION)
string(STRIP "${FGC_PROJECT_NAME}" FGC_PROJECT_NAME)
string(STRIP "${FGC_PROJECT_VERSION}" FGC_PROJECT_VERSION)
if(NOT FGC_PROJECT_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "Version '${FGC_PROJECT_VERSION}' in name-version.txt must look like 1.2.3")
endif()
set(FGC_VERSION_MAJOR ${CMAKE_MATCH_1})
set(FGC_VERSION_MINOR ${CMAKE_MATCH_2})
set(FGC_VERSION_PATCH ${CMAKE_MATCH_3})

project(FgcKeyboardSplitter VERSION ${FGC_PROJECT_VERSION} LANGUAGES CXX RC)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT so the binaries run on machines without the Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Keep FgcApp.exe and FgcCore.dll side by side so the exe finds the dll.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# fgc_version.h for code that shows the version, and a version resource per binary.
configure_file(cmake/fgc_version.h.in "${CMAKE_BINARY_DIR}/generated/fgc_version.h" @ONLY)
add_library(fgc_version INTERFACE)
target_include_directories(fgc_version INTERFACE "${CMAKE_BINARY_DIR}/generated")

# Adds the version resource to a target: fgc_add_version_resource(<target> <file type> <file name> <description>)
function(fgc_add_version_resource target filetype filename description)
    set(FGC_RC_FILETYPE "${filetype}")
    set(FGC_RC_FILENAME "${filename}")
    set(FGC_RC_DESCRIPTION "${description}")
    configure_file(${PROJECT_SOURCE_DIR}/cmake/version.rc.in "${CMAKE_BINARY_DIR}/generated/${target}_version.rc" @ONLY)
    target_sources(${target} PRIVATE "${CMAKE_BINARY_DIR}/generated/${target}_version.rc")
endfunction()

add_subdirectory(core)
add_subdirectory(app)
add_subdirectory(cli)

# `cmake --build build --config Release --target package` makes FgcKeyboardSplitter-<version>-win-x64.zip.
install(TARGETS FgcApp FgcCli RUNTIME DESTINATION .)
install(FILES $<TARGET_FILE:FgcCore> README.md DESTINATION .)
install(DIRECTORY drivers DESTINATION .)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME "FgcKeyboardSplitter")
set(CPACK_PACKAGE_VERSION "${FGC_PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "FgcKeyboardSplitter-${FGC_PROJECT_VERSION}-win-x64")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
