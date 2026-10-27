cmake_minimum_required(VERSION 3.30)
project(TriAevumQuestRuntime LANGUAGES C CXX)
if(NOT ANDROID OR NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "Use the Android NDK arm64-v8a toolchain")
endif()
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(OOT3D_WHOLE_AOT_PRODUCT_MODE ON CACHE BOOL "" FORCE)
set(OOT3D_DIRECT_AOT_PLUGIN ON CACHE BOOL "" FORCE)
set(OOT3D_ENABLE_VULKAN_RENDERER ON CACHE BOOL "" FORCE)
set(THREE_DS_RECOMP_ENABLE_CACAO OFF CACHE BOOL "" FORCE)
set(THREE_DS_RECOMP_ENABLE_SSSR OFF CACHE BOOL "" FORCE)
set(USE_OPENGLES ON CACHE BOOL "" FORCE)
set(ENABLE_SCRIPTING OFF CACHE BOOL "" FORCE)
set(TRIAEVUM_ANDROID_DEPENDENCY_SOURCE_CACHE "" CACHE PATH "Existing FetchContent source trees (never share binary directories)")
if(TRIAEVUM_ANDROID_DEPENDENCY_SOURCE_CACHE)
    file(GLOB cached_sources LIST_DIRECTORIES TRUE "${TRIAEVUM_ANDROID_DEPENDENCY_SOURCE_CACHE}/*-src")
    foreach(cached_source IN LISTS cached_sources)
        get_filename_component(dependency "${cached_source}" NAME)
        string(REGEX REPLACE "-src$" "" dependency "${dependency}")
        string(TOUPPER "${dependency}" dependency)
        set("FETCHCONTENT_SOURCE_DIR_${dependency}" "${cached_source}" CACHE PATH "Cached dependency source" FORCE)
    endforeach()
endif()

# Immersive mode (TriAevumXrActivity): OpenXR on the renderer's Vulkan device. The definition is
# set here, for every target of the product tree, so class layouts guarded by it stay consistent;
# the panel activity never selects XR at run time (TRIAEVUM_XR=1 is set only by the XR activity).
option(TRIAEVUM_QUEST_OPENXR "Immersive OpenXR mode (TriAevumXrActivity)" ON)
if(TRIAEVUM_QUEST_OPENXR)
    add_compile_definitions(TRIAEVUM_QUEST_OPENXR=1)
    include_directories("${CMAKE_CURRENT_SOURCE_DIR}/xr")
    # Khronos OpenXR loader for Android (the pin and hash of ports/quest/hello and Xelee's aurora).
    # Kept as a *-src tree so the CI dependency-source cache carries it; the .aar is re-hashed always.
    set(OPENXR_ANDROID_VERSION "1.1.63")
    set(OPENXR_ANDROID_SHA256 "622419d2f6741c3443a3beb4779af0764318edd01830de967f24c741ebcded73")
    set(_xr_aar_name "openxr_loader_for_android-${OPENXR_ANDROID_VERSION}.aar")
    set(_xr_dir "${CMAKE_BINARY_DIR}/_deps/openxr_loader_android-src")
    if(FETCHCONTENT_SOURCE_DIR_OPENXR_LOADER_ANDROID AND
       EXISTS "${FETCHCONTENT_SOURCE_DIR_OPENXR_LOADER_ANDROID}/${_xr_aar_name}")
        set(_xr_dir "${FETCHCONTENT_SOURCE_DIR_OPENXR_LOADER_ANDROID}")
    endif()
    set(_xr_aar "${_xr_dir}/${_xr_aar_name}")
    if(NOT EXISTS "${_xr_aar}")
        set(_xr_url "https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/${OPENXR_ANDROID_VERSION}/${_xr_aar_name}")
        message(STATUS "Fetching the OpenXR loader ${OPENXR_ANDROID_VERSION}")
        file(MAKE_DIRECTORY "${_xr_dir}")
        file(DOWNLOAD "${_xr_url}" "${_xr_aar}" EXPECTED_HASH SHA256=${OPENXR_ANDROID_SHA256} STATUS _xr_status)
        list(GET _xr_status 0 _xr_code)
        if(NOT _xr_code EQUAL 0)
            file(REMOVE "${_xr_aar}")
            message(FATAL_ERROR "Downloading ${_xr_url} failed: ${_xr_status}")
        endif()
    endif()
    file(SHA256 "${_xr_aar}" _xr_hash)
    if(NOT _xr_hash STREQUAL OPENXR_ANDROID_SHA256)
        message(FATAL_ERROR "${_xr_aar}: SHA-256 ${_xr_hash}, expected ${OPENXR_ANDROID_SHA256}")
    endif()
    set(_xr_so "${_xr_dir}/prefab/modules/openxr_loader/libs/android.${CMAKE_ANDROID_ARCH_ABI}/libopenxr_loader.so")
    # The loader's licence (Apache-2.0 OR MIT) ships with it: APK assets/licenses, tools LICENSES/.
    set(_xr_license "${_xr_dir}/META-INF/LICENSE")
    if(NOT EXISTS "${_xr_so}" OR NOT EXISTS "${_xr_license}")
        file(ARCHIVE_EXTRACT INPUT "${_xr_aar}" DESTINATION "${_xr_dir}")
    endif()
    if(NOT EXISTS "${_xr_license}")
        message(FATAL_ERROR "${_xr_aar}: no META-INF/LICENSE")
    endif()
    add_library(openxr_loader SHARED IMPORTED GLOBAL)
    set_target_properties(openxr_loader PROPERTIES
        IMPORTED_LOCATION "${_xr_so}"
        IMPORTED_NO_SONAME TRUE
        INTERFACE_INCLUDE_DIRECTORIES "${_xr_dir}/prefab/modules/headers/include")
    # The staging step (tools/quest/stage_quest.py --openxr) packages it from here.
    configure_file("${_xr_so}" "${CMAKE_BINARY_DIR}/openxr/libopenxr_loader.so" COPYONLY)
    configure_file("${_xr_license}" "${CMAKE_BINARY_DIR}/openxr/LICENSE.txt" COPYONLY)
endif()

get_filename_component(root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_subdirectory("${root}" product)
# The Android host (app-local paths and logs) is shared with ports/android; the Quest shell adds
# its process-exit entry point. The title module is never built here: it is side-loaded.
target_sources(oot3d_native_game PRIVATE "${root}/ports/android/runtime/android_host.cpp" quest_host.cpp)
target_include_directories(oot3d_native_game PRIVATE "${root}/ports/android/runtime")
target_link_libraries(oot3d_native_game PRIVATE log)

if(TRIAEVUM_QUEST_OPENXR)
    # In the runtime library, because the Vulkan backend (part of it) calls the XR sink; every
    # binary linking it then resolves the symbols.
    target_sources(three_ds_recomp_runtime PRIVATE
        xr/quest_xr_runtime.cpp xr/quest_xr_vulkan.cpp xr/quest_xr_input.cpp)
    target_link_libraries(three_ds_recomp_runtime PUBLIC openxr_loader android log)
    # Pure chord, latch and placement logic (adb-runnable, in quest-tools-arm64.zip).
    add_executable(quest_xr_logic_tests EXCLUDE_FROM_ALL xr/quest_xr_logic_tests.cpp)
    set_target_properties(quest_xr_logic_tests PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/product")
endif()
