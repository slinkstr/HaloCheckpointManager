configure_file(HCMSpeedhack.rc.in "${CMAKE_CURRENT_BINARY_DIR}/HCMSpeedhack.rc" @ONLY)

add_library(HCMSpeedhack SHARED
    dllmain.cpp
    Speedhack.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/HCMSpeedhack.rc"
)

target_precompile_headers(HCMSpeedhack PRIVATE pch.h)

target_compile_options(HCMSpeedhack PRIVATE /W3 /sdl /EHsc /permissive- /std:c++latest)

target_compile_definitions(HCMSpeedhack PRIVATE
    _WINDOWS
    _USRDLL
    UNICODE
    _UNICODE
    HCMSPEEDHACK_EXPORTS
)

target_include_directories(HCMSpeedhack PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

find_package(zydis CONFIG REQUIRED)

target_link_libraries(HCMSpeedhack PRIVATE
    safetyhook::safetyhook
    Zydis::Zydis
)

set_target_properties(HCMSpeedhack PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${HCM_OUTPUT_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
