cmake_minimum_required(VERSION 3.22)
project(Lilac2LilyMod CXX C)

if(NOT MSVC)
    message(FATAL_ERROR "Lilac2LilyMod supports only CMake + MSVC/MSBuild. Configure with Visual Studio 17 2022 x64.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

if(MSVC)
    add_compile_options(/utf-8)
    add_compile_options(
        /permissive-
        /Zc:preprocessor /Zc:lambda /Zc:strictStrings /Zc:inline
        /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR
        /wd4005 /wd4251 /wd4068 /wd4201
    )
    add_compile_options(/experimental:external /external:W0)
endif()

set(UE4SS_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ue4ss-sdk")
if(NOT EXISTS "${UE4SS_SDK_DIR}/VERSION")
    message(FATAL_ERROR "UE4SS SDK not found at ${UE4SS_SDK_DIR}")
endif()
file(READ "${UE4SS_SDK_DIR}/VERSION" _SDK_VER_TEXT)
message(STATUS "Locked UE4SS SDK:\n${_SDK_VER_TEXT}")
include("${UE4SS_SDK_DIR}/cmake/ue4ss-sdk-defines.cmake")

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
    "${UE4SS_SDK_DIR}/include"
    "${UE4SS_SDK_DIR}/include/Unreal"
    "${UE4SS_SDK_DIR}/include/Unreal/Core"
)

target_include_directories(Lilac2LilyMod SYSTEM PRIVATE
    "${UE4SS_SDK_DIR}/include/fmt"
    "${UE4SS_SDK_DIR}/include/glaze"
    "${UE4SS_SDK_DIR}/include/Zydis"
    "${UE4SS_SDK_DIR}/include/imgui"
    "${UE4SS_SDK_DIR}/include/polyhook2"
)

file(GLOB UE4SS_SDK_LIBS "${UE4SS_SDK_DIR}/lib/*.lib")
target_link_libraries(Lilac2LilyMod PRIVATE
    ${UE4SS_SDK_LIBS}
    d3d11 dbghelp psapi ws2_32 ntdll userenv opengl32 windowscodecs
)

set_target_properties(Lilac2LilyMod PROPERTIES
    OUTPUT_NAME "main"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

add_custom_command(TARGET Lilac2LilyMod POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/config"
            "$<TARGET_FILE_DIR:Lilac2LilyMod>/config"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:Lilac2LilyMod>/assets"
)


