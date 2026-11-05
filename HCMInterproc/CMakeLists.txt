configure_file(HCMInterproc.rc.in "${CMAKE_CURRENT_BINARY_DIR}/HCMInterproc.rc" @ONLY)

add_library(HCMInterproc SHARED
    dllmain.cpp
    Events.cpp
    FindProcess.cpp
    HookStateMachine.cpp
    ImbueInternal.cpp
    InitInterproc.cpp
    SharedMemoryExternal.cpp
    WindowsUtilities.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/HCMInterproc.rc"
)

target_precompile_headers(HCMInterproc PRIVATE pch.h)

target_compile_options(HCMInterproc PRIVATE /W3 /sdl /EHsc /permissive- /std:c++latest)

target_compile_definitions(HCMInterproc PRIVATE
    _WINDOWS
    _USRDLL
    UNICODE
    _UNICODE
    HCMINTERPROC_EXPORTS
    _SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING
    $<$<CONFIG:Debug>:HCM_DEBUG>
)

target_include_directories(HCMInterproc PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

find_package(plog CONFIG REQUIRED)
find_package(magic_enum CONFIG REQUIRED)
find_package(boost_interprocess CONFIG REQUIRED)

target_link_libraries(HCMInterproc PRIVATE
    plog::plog
    magic_enum::magic_enum
    Boost::interprocess
)

set_target_properties(HCMInterproc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${HCM_OUTPUT_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
