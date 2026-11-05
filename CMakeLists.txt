cmake_minimum_required(VERSION 3.28)

# Resolve the vcpkg toolchain before project(): env var, vendored submodule, PATH.
# Must run before project() so the toolchain applies to compiler detection.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(_vcpkg_candidates "$ENV{VCPKG_ROOT}" "${CMAKE_SOURCE_DIR}/vcpkg")
    find_program(_vcpkg_exe vcpkg)
    if(_vcpkg_exe)
        cmake_path(GET _vcpkg_exe PARENT_PATH _vcpkg_bin)
        list(APPEND _vcpkg_candidates "${_vcpkg_bin}")
    endif()
    foreach(_root IN LISTS _vcpkg_candidates)
        if(EXISTS "${_root}/scripts/buildsystems/vcpkg.cmake")
            set(CMAKE_TOOLCHAIN_FILE "${_root}/scripts/buildsystems/vcpkg.cmake")
            break()
        endif()
    endforeach()
    unset(_vcpkg_candidates)
    unset(_vcpkg_exe)
    unset(_vcpkg_bin)
    if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
        message(FATAL_ERROR "vcpkg toolchain not found - set VCPKG_ROOT, vendor vcpkg/ as a submodule, or put vcpkg.exe on PATH")
    endif()
endif()

project(HaloCheckpointManager LANGUAGES C CXX RC)

# Single version source shared with HCMExternal
file(STRINGS "${CMAKE_SOURCE_DIR}/Directory.Build.props" _version_line REGEX "<VersionPrefix>")
string(REGEX MATCH ">([0-9.]+)<" _ "${_version_line}")
set(HCM_VERSION "${CMAKE_MATCH_1}")
string(REPLACE "." "," HCM_VERSION_COMMA "${HCM_VERSION}")
if(NOT HCM_VERSION)
    message(FATAL_ERROR "Could not read VersionPrefix from Directory.Build.props")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/Directory.Build.props")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "HCM targets x64 only")
endif()

# Static CRT to match the x64-windows-static triplet
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,Release>:Embedded>")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# /DEBUG implies /OPT:NOREF,NOICF - restore dead-code elimination for Release
add_link_options("$<$<CONFIG:Release>:/DEBUG>" "$<$<CONFIG:Release>:/OPT:REF>" "$<$<CONFIG:Release>:/OPT:ICF>")

# vcpkg manifest mode installs under the build dir. Header-only ports have no
# config packages, so the triplet include dir goes on the global path - same
# as MSBuild vcpkg auto-integration did.
include_directories(SYSTEM "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include")

# All native outputs land next to the C# app, matching the old OutDir redirect
set(HCM_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/HCMExternal/bin/x64/$<CONFIG>/net7.0-windows")

# safetyhook links a bare "Zydis" name (its fetch option is off); point it at the
# real vcpkg target so consumers don't get an unresolved Zydis.lib on the link line
find_package(zydis CONFIG REQUIRED)
add_library(Zydis ALIAS Zydis::Zydis)

add_subdirectory(HCMInternal/includes/safetyhook)
add_subdirectory(HCMInterproc)
add_subdirectory(HCMSpeedhack)
add_subdirectory(HCMInternal)
