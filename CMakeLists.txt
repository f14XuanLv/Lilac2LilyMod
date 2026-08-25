cmake_minimum_required(VERSION 3.22)
project(Lilac2LilyMod CXX C)

if(NOT MSVC)
    message(FATAL_ERROR "Lilac2LilyMod supports only CMake + MSVC/MSBuild. Configure with Visual Studio 17 2022 x64.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build against the pinned RE-UE4SS submodule. The UE4SS target exports the
# official C++ mod headers and libraries to external consumers.
add_subdirectory(RE-UE4SS)

include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.3
)
FetchContent_GetProperties(minhook)
if(NOT minhook_POPULATED)
    FetchContent_Populate(minhook)
endif()

set(MINHOOK_SOURCES
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c
)

add_library(Lilac2LilyMod SHARED
    src/dllmain.cpp
    src/lifecycle.cpp
    src/rt_config.cpp
    src/costume.cpp
    src/spine.cpp
    src/rva_check.cpp
    src/asset_forge/uobject_copy_forge.cpp
    src/asset_forge/paper_sprite_forge.cpp
    src/asset_forge/texture2d_forge.cpp
    src/asset_forge/spine_atlas_forge.cpp
    src/asset_forge/spine_skeleton_data_forge.cpp
    ${MINHOOK_SOURCES}
)

target_include_directories(Lilac2LilyMod PRIVATE
    src
    ${minhook_SOURCE_DIR}/include
)

target_link_libraries(Lilac2LilyMod PUBLIC UE4SS)
target_link_libraries(Lilac2LilyMod PRIVATE d3d11 windowscodecs)

set(LILAC2LILY_MOD_PACKAGE_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>/Lilac2LilyMod")

set_target_properties(Lilac2LilyMod PROPERTIES
    OUTPUT_NAME "main"
    RUNTIME_OUTPUT_DIRECTORY "${LILAC2LILY_MOD_PACKAGE_DIR}/dlls"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

# Package the mod in the directory structure expected by UE4SS.
add_custom_command(TARGET Lilac2LilyMod POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${LILAC2LILY_MOD_PACKAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/config"
            "${LILAC2LILY_MOD_PACKAGE_DIR}/config"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "${LILAC2LILY_MOD_PACKAGE_DIR}/assets"
)
