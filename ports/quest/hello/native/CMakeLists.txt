# TriAevum XR Test (Quest milestone Q1): libtriaevum_hello.so (SDL_main + the
# XR thread), SDL2 2.32.10 from source as upstream's Android port uses it, the
# Khronos OpenXR loader from its Maven AAR, and libprobe.so, the title-free
# dummy for the side-load probe (shipped beside the APK, never inside it).
cmake_minimum_required(VERSION 3.22)
project(triaevum_quest_hello C CXX)

if (NOT ANDROID)
    message(FATAL_ERROR "Configure with the Android NDK toolchain (arm64-v8a).")
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Every shared library (ours and SDL2's) lands in lib/ for APK staging.
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

include(FetchContent)

# ---- SDL2 (same pin as runtime/three_ds_recomp/cmake/dependencies/android.cmake)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.32.10
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL2)
# The Gradle build compiles SDLActivity from this exact source.
file(WRITE "${CMAKE_BINARY_DIR}/sdl-source-dir.txt" "${sdl2_SOURCE_DIR}")

# ---- Khronos OpenXR loader for Android (pin and hash as Xelee's aurora)
set(OPENXR_ANDROID_VERSION "1.1.63")
set(OPENXR_ANDROID_SHA256 "622419d2f6741c3443a3beb4779af0764318edd01830de967f24c741ebcded73")
set(_xr_dir "${CMAKE_BINARY_DIR}/_deps/openxr_loader_android-${OPENXR_ANDROID_VERSION}")
set(_xr_aar "${_xr_dir}.aar")
if (NOT EXISTS "${_xr_dir}/prefab")
    set(_xr_url "https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/${OPENXR_ANDROID_VERSION}/openxr_loader_for_android-${OPENXR_ANDROID_VERSION}.aar")
    message(STATUS "Fetching OpenXR loader ${OPENXR_ANDROID_VERSION}")
    file(DOWNLOAD "${_xr_url}" "${_xr_aar}" EXPECTED_HASH SHA256=${OPENXR_ANDROID_SHA256} STATUS _xr_status)
    list(GET _xr_status 0 _xr_code)
    if (NOT _xr_code EQUAL 0)
        message(FATAL_ERROR "Downloading ${_xr_url} failed: ${_xr_status}")
    endif ()
    file(ARCHIVE_EXTRACT INPUT "${_xr_aar}" DESTINATION "${_xr_dir}")
endif ()
set(_xr_so "${_xr_dir}/prefab/modules/openxr_loader/libs/android.${ANDROID_ABI}/libopenxr_loader.so")
add_library(openxr_loader SHARED IMPORTED)
set_target_properties(openxr_loader PROPERTIES
    IMPORTED_LOCATION "${_xr_so}"
    IMPORTED_NO_SONAME TRUE
    INTERFACE_INCLUDE_DIRECTORIES "${_xr_dir}/prefab/modules/headers/include")
configure_file("${_xr_so}" "${CMAKE_BINARY_DIR}/lib/libopenxr_loader.so" COPYONLY)

# ---- shaders: the NDK's glslc, SPIR-V embedded as number lists
file(GLOB _glslc_candidates "${ANDROID_NDK}/shader-tools/*/glslc" "${ANDROID_NDK}/shader-tools/*/glslc.exe")
if (_glslc_candidates)
    list(GET _glslc_candidates 0 GLSLC)
else ()
    find_program(GLSLC NAMES glslc)
endif ()
if (NOT GLSLC)
    message(FATAL_ERROR "glslc not found under ${ANDROID_NDK}/shader-tools")
endif ()
set(_spv)
foreach (_stage vert frag)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene.${_stage}")
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/scene.${_stage}.inc")
    add_custom_command(OUTPUT "${_out}"
        COMMAND "${GLSLC}" --target-env=vulkan1.1 -O -mfmt=num -o "${_out}" "${_src}"
        DEPENDS "${_src}"
        COMMENT "glslc scene.${_stage}")
    list(APPEND _spv "${_out}")
endforeach ()

# ---- the app's native library
add_library(triaevum_hello SHARED hello_xr.cpp ${_spv})
target_include_directories(triaevum_hello PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(triaevum_hello PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter)
if (TARGET SDL2::SDL2)
    target_link_libraries(triaevum_hello PRIVATE SDL2::SDL2)
else ()
    target_link_libraries(triaevum_hello PRIVATE SDL2)
endif ()
target_link_libraries(triaevum_hello PRIVATE openxr_loader vulkan android log dl)

# ---- the side-load probe (title-free dummy; plain C, no libc++)
add_library(triaevum_probe SHARED probe/probe.c)
set_target_properties(triaevum_probe PROPERTIES OUTPUT_NAME probe)
