cmake_minimum_required(VERSION 3.16)
project(mali_wrapper VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Auto-detect target architecture from toolchain
if(CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE MATCHES "armhf")
    set(BUILD_32BIT ON CACHE BOOL "Build 32-bit wrapper" FORCE)
    set(BUILD_64BIT OFF CACHE BOOL "Build 64-bit wrapper" FORCE)
    message(STATUS "Auto-detected: 32-bit cross-compilation build")
else()
    set(BUILD_64BIT ON CACHE BOOL "Build 64-bit wrapper" FORCE)
    set(BUILD_32BIT OFF CACHE BOOL "Build 32-bit wrapper" FORCE)
    message(STATUS "Auto-detected: 64-bit native build")
endif()

# Build options (can still be overridden manually)
option(BUILD_32BIT "Build 32-bit wrapper" ${BUILD_32BIT})
option(BUILD_64BIT "Build 64-bit wrapper" ${BUILD_64BIT})
option(INSTALL_ICDS "Install ICD manifests" ON)
option(BUILD_HUD "Build the integrated opt-in Mali HUD" ON)
option(MALI_HUD_TEST_BUILD "Enable HUD-only fault injection hooks" OFF)

include(CTest)

execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE MALI_WRAPPER_GIT_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT MALI_WRAPPER_GIT_REVISION)
    set(MALI_WRAPPER_GIT_REVISION "unknown")
endif()

# WSI configuration options
option(BUILD_WSI_X11 "Enable X11 WSI support" ON)
option(BUILD_WSI_WAYLAND "Enable Wayland WSI support" ON)
option(BUILD_WSI_HEADLESS "Enable headless WSI support" ON)
option(ENABLE_WAYLAND_FIFO_PRESENTATION_THREAD "Enable Wayland FIFO presentation thread" ON)
set(SELECT_EXTERNAL_ALLOCATOR "dma_buf_heaps" CACHE STRING "External allocator backend for wsialloc")
set(WSIALLOC_MEMORY_HEAP_NAME "system-uncached" CACHE STRING "DMA-BUF heap used by wsialloc")

# Path configuration options
set(MALI_DRIVER_PATH_64 "/usr/lib/aarch64-linux-gnu/libmali.so"
    CACHE STRING "Path to 64-bit Mali driver")
set(MALI_DRIVER_PATH_32 "/usr/lib/arm-linux-gnueabihf/libmali.so"
    CACHE STRING "Path to 32-bit Mali driver")

# API version
set(API_VERSION "1.3.276" CACHE STRING "Vulkan API version")

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(VULKAN REQUIRED vulkan)

if(BUILD_HUD)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(GLSLANG_VALIDATOR_EXECUTABLE glslangValidator)
    find_path(STB_INCLUDE_DIR stb/stb_truetype.h)
    if(NOT GLSLANG_VALIDATOR_EXECUTABLE)
        message(FATAL_ERROR "BUILD_HUD requires glslangValidator (install glslang-tools)")
    endif()
    if(NOT STB_INCLUDE_DIR)
        message(FATAL_ERROR "BUILD_HUD requires stb/stb_truetype.h (install libstb-dev)")
    endif()
    set(HUD_FONT_FILE "" CACHE FILEPATH "JetBrains Mono Regular TTF embedded in the HUD")
    if(NOT HUD_FONT_FILE)
        # Search into a separate variable: find_file() skips searching when its
        # result variable is already cached as an empty string.
        unset(HUD_FONT_DISCOVERED CACHE)
        find_file(HUD_FONT_DISCOVERED
            NAMES JetBrainsMono-Regular.ttf
            PATHS
                /usr/share/fonts/truetype/jetbrains-mono
                /usr/share/fonts/opentype/jetbrains-mono
                /usr/local/share/fonts
                $ENV{HOME}/.local/share/fonts
            NO_DEFAULT_PATH
        )
        if(HUD_FONT_DISCOVERED)
            set(HUD_FONT_FILE "${HUD_FONT_DISCOVERED}" CACHE FILEPATH
                "JetBrains Mono Regular TTF embedded in the HUD" FORCE)
        endif()
    endif()
    if(NOT HUD_FONT_FILE OR NOT EXISTS "${HUD_FONT_FILE}")
        message(FATAL_ERROR "BUILD_HUD requires JetBrainsMono-Regular.ttf (install fonts-jetbrains-mono or set HUD_FONT_FILE)")
    endif()

    set(HUD_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/hud)
    set(HUD_FONT_HEADER ${HUD_GENERATED_DIR}/hud_font_data.hpp)
    set(HUD_VERTEX_HEADER ${HUD_GENERATED_DIR}/hud_vert_spv.hpp)
    set(HUD_FRAGMENT_HEADER ${HUD_GENERATED_DIR}/hud_frag_spv.hpp)

    add_custom_command(
        OUTPUT ${HUD_FONT_HEADER}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_binary.py
                ${HUD_FONT_FILE} ${HUD_FONT_HEADER} mali_hud_font_data
        DEPENDS ${HUD_FONT_FILE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_binary.py
        VERBATIM
    )
    add_custom_command(
        OUTPUT ${HUD_VERTEX_HEADER}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${HUD_GENERATED_DIR}
        COMMAND ${GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.0
                --vn mali_hud_vert_spv -o ${HUD_VERTEX_HEADER}
                ${CMAKE_CURRENT_SOURCE_DIR}/src/hud/shaders/hud.vert
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/hud/shaders/hud.vert
        VERBATIM
    )
    add_custom_command(
        OUTPUT ${HUD_FRAGMENT_HEADER}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${HUD_GENERATED_DIR}
        COMMAND ${GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.0
                --vn mali_hud_frag_spv -o ${HUD_FRAGMENT_HEADER}
                ${CMAKE_CURRENT_SOURCE_DIR}/src/hud/shaders/hud.frag
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/hud/shaders/hud.frag
        VERBATIM
    )
    add_custom_target(hud_generated_files DEPENDS ${HUD_FONT_HEADER} ${HUD_VERTEX_HEADER} ${HUD_FRAGMENT_HEADER})
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/JetBrainsMono-OFL.txt
        DESTINATION share/doc/mali-wrapper)
endif()

# WSI dependencies
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(LIBDRM REQUIRED libdrm)
find_package(X11 REQUIRED)

# Find XCB
pkg_check_modules(XCB REQUIRED xcb xcb-dri3 xcb-present xcb-shm xcb-sync)

# Find Xrandr for X11
find_library(XRANDR_LIBRARY Xrandr REQUIRED)

# Generate Wayland protocol headers
find_program(WAYLAND_SCANNER_EXEC wayland-scanner REQUIRED)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
# Use cached value from toolchain if set, otherwise query pkg-config
if(NOT WAYLAND_PROTOCOLS_DIR)
    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
endif()

add_custom_target(wayland_generated_files
    COMMAND ${WAYLAND_SCANNER_EXEC} client-header
    ${WAYLAND_PROTOCOLS_DIR}/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
    ${CMAKE_CURRENT_BINARY_DIR}/linux-dmabuf-unstable-v1-client-protocol.h
    COMMAND ${WAYLAND_SCANNER_EXEC} public-code
    ${WAYLAND_PROTOCOLS_DIR}/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
    ${CMAKE_CURRENT_BINARY_DIR}/linux-dmabuf-unstable-v1-protocol.c
    COMMAND ${WAYLAND_SCANNER_EXEC} client-header
    ${WAYLAND_PROTOCOLS_DIR}/unstable/linux-explicit-synchronization/linux-explicit-synchronization-unstable-v1.xml
    ${CMAKE_CURRENT_BINARY_DIR}/linux-explicit-synchronization-unstable-v1-protocol.h
    COMMAND ${WAYLAND_SCANNER_EXEC} public-code
    ${WAYLAND_PROTOCOLS_DIR}/unstable/linux-explicit-synchronization/linux-explicit-synchronization-unstable-v1.xml
    ${CMAKE_CURRENT_BINARY_DIR}/linux-explicit-synchronization-unstable-v1-protocol.c
    COMMAND ${WAYLAND_SCANNER_EXEC} client-header
    ${WAYLAND_PROTOCOLS_DIR}/stable/presentation-time/presentation-time.xml
    ${CMAKE_CURRENT_BINARY_DIR}/presentation-time-client-protocol.h
    COMMAND ${WAYLAND_SCANNER_EXEC} public-code
    ${WAYLAND_PROTOCOLS_DIR}/stable/presentation-time/presentation-time.xml
    ${CMAKE_CURRENT_BINARY_DIR}/presentation-time-client-protocol.c
    BYPRODUCTS linux-dmabuf-unstable-v1-protocol.c linux-dmabuf-unstable-v1-client-protocol.h
               linux-explicit-synchronization-unstable-v1-protocol.c linux-explicit-synchronization-unstable-v1-protocol.h
               presentation-time-client-protocol.c presentation-time-client-protocol.h)

# WSI source files
set(WSI_SOURCES
    src/core/wsi_manager.cpp
    src/wsi/wsi_factory.cpp
    src/wsi/wsi_private_data.cpp
    src/wsi/swapchain_base.cpp
    src/wsi/surface_properties.cpp
    src/wsi/external_memory.cpp
    src/wsi/synchronization.cpp
    src/wsi/swapchain_api.cpp
    src/wsi/surface_api.cpp
    src/wsi/layer_utils/extension_list.cpp
    src/wsi/layer_utils/custom_allocator.cpp
    src/wsi/layer_utils/timed_semaphore.cpp
    src/wsi/layer_utils/format_modifiers.cpp
)

# Platform-specific WSI sources (X11)
set(WSI_X11_SOURCES
    src/wsi/x11/surface.cpp
    src/wsi/x11/surface_properties.cpp
    src/wsi/x11/swapchain.cpp
    src/wsi/x11/shm_presenter.cpp
    src/wsi/x11/dri3_presenter.cpp
    src/wsi/x11/drm_display.cpp
    src/wsi/x11/xwayland_dmabuf_bridge.cpp
)

# Platform-specific WSI sources (Wayland)
set(WSI_WAYLAND_SOURCES
    src/wsi/wayland/surface.cpp
    src/wsi/wayland/surface_properties.cpp
    src/wsi/wayland/swapchain.cpp
    src/wsi/wayland/wl_helpers.cpp
)

# Platform-specific WSI sources (Headless)
set(WSI_HEADLESS_SOURCES
    src/wsi/headless/surface.cpp
    src/wsi/headless/surface_properties.cpp
    src/wsi/headless/swapchain.cpp
)

# WSI extension sources
set(WSI_EXTENSION_SOURCES
    src/wsi/extensions/wsi_extension.cpp
    src/wsi/extensions/image_compression_control.cpp
    src/wsi/extensions/present_id.cpp
    src/wsi/extensions/frame_boundary.cpp
    src/wsi/extensions/swapchain_maintenance.cpp
)

set(HUD_SOURCES)
if(BUILD_HUD)
    list(APPEND HUD_SOURCES
        src/hud/hud_config.cpp
        src/hud/hud_metadata.cpp
        src/hud/hud_metrics.cpp
        src/hud/hud_renderer.cpp
        src/hud/hud_runtime.cpp
    )
endif()

# Common source files
set(COMMON_SOURCES
    src/icd_main.cpp
    src/core/mali_wrapper_icd.cpp
    src/core/library_loader.cpp
    src/utils/logging.cpp
    ${WSI_SOURCES}
    ${WSI_X11_SOURCES}
    ${WSI_WAYLAND_SOURCES}
    ${WSI_HEADLESS_SOURCES}
    ${WSI_EXTENSION_SOURCES}
    ${HUD_SOURCES}
)

# Generated Wayland protocol files (compiled as C, not C++)
set(WAYLAND_PROTOCOL_SOURCES
    ${CMAKE_CURRENT_BINARY_DIR}/linux-dmabuf-unstable-v1-protocol.c
    ${CMAKE_CURRENT_BINARY_DIR}/linux-explicit-synchronization-unstable-v1-protocol.c
    ${CMAKE_CURRENT_BINARY_DIR}/presentation-time-client-protocol.c
)
set_source_files_properties(${WAYLAND_PROTOCOL_SOURCES} PROPERTIES LANGUAGE C)

# DRM utility source files (some as C++)
set(DRM_SOURCES
    src/wsi/layer_utils/drm/drm_utils.cpp
    src/wsi/layer_utils/drm/format_table.c
)
set_source_files_properties(src/wsi/layer_utils/drm/format_table.c PROPERTIES LANGUAGE C)

# WSI allocator source files (compiled as C)
set(WSIALLOC_SOURCES
    src/wsi/layer_utils/wsialloc/wsialloc_helpers.c
    src/wsi/layer_utils/wsialloc/wsialloc_dma_buf_heaps.c
)
set_source_files_properties(${WSIALLOC_SOURCES} PROPERTIES LANGUAGE C)

# Boolean compile-definition helpers
set(BUILD_WSI_X11_DEFINE 0)
set(BUILD_WSI_WAYLAND_DEFINE 0)
set(BUILD_WSI_HEADLESS_DEFINE 0)
set(ENABLE_WAYLAND_FIFO_PRESENTATION_THREAD_DEFINE 0)

if(BUILD_WSI_X11)
    set(BUILD_WSI_X11_DEFINE 1)
endif()

if(BUILD_WSI_WAYLAND)
    set(BUILD_WSI_WAYLAND_DEFINE 1)
endif()

if(BUILD_WSI_HEADLESS)
    set(BUILD_WSI_HEADLESS_DEFINE 1)
endif()

if(ENABLE_WAYLAND_FIFO_PRESENTATION_THREAD)
    set(ENABLE_WAYLAND_FIFO_PRESENTATION_THREAD_DEFINE 1)
endif()

# Common include directories
set(COMMON_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/wsi/layer_utils/drm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/wsi/layer_utils/wsialloc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/wsi/layer_utils
    ${CMAKE_CURRENT_BINARY_DIR}
    ${VULKAN_INCLUDE_DIRS}
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${LIBDRM_INCLUDE_DIRS}
    ${X11_INCLUDE_DIR}
    ${XCB_INCLUDE_DIRS}
    /usr/include
    /usr/include/drm
)
if(BUILD_HUD)
    list(APPEND COMMON_INCLUDES ${STB_INCLUDE_DIR} ${HUD_GENERATED_DIR})
endif()

# Common compile definitions
set(COMMON_DEFINITIONS
    VK_USE_PLATFORM_XLIB_KHR
    VK_USE_PLATFORM_XCB_KHR
    VK_USE_PLATFORM_WAYLAND_KHR
    BUILD_WSI_X11=${BUILD_WSI_X11_DEFINE}
    BUILD_WSI_WAYLAND=${BUILD_WSI_WAYLAND_DEFINE}
    BUILD_WSI_HEADLESS=${BUILD_WSI_HEADLESS_DEFINE}
    VULKAN_WSI_LAYER_EXPERIMENTAL=0
    ENABLE_INSTRUMENTATION=0
    WAYLAND_FIFO_PRESENTATION_THREAD_ENABLED=${ENABLE_WAYLAND_FIFO_PRESENTATION_THREAD_DEFINE}
    SELECT_EXTERNAL_ALLOCATOR=${SELECT_EXTERNAL_ALLOCATOR}
    WSIALLOC_MEMORY_HEAP_NAME=${WSIALLOC_MEMORY_HEAP_NAME}
    ENABLE_ARM_NEON=1
    BUILD_HUD=$<BOOL:${BUILD_HUD}>
    MALI_HUD_TEST_BUILD=$<BOOL:${MALI_HUD_TEST_BUILD}>
)

# Common link libraries
set(COMMON_LIBRARIES
    ${CMAKE_DL_LIBS}
    ${WAYLAND_CLIENT_LIBRARIES}
    ${LIBDRM_LIBRARIES}
    ${X11_LIBRARIES}
    ${X11_Xrandr_LIB}
    ${XCB_LIBRARIES}
    X11-xcb
    xcb-dri3
    xcb-present
    xcb-shm
    xcb-sync
    drm
    pthread
)

# Function to create a wrapper target
function(create_wrapper_target ARCH_NAME ARCH_FLAGS)
    set(TARGET_NAME mali_wrapper_${ARCH_NAME})

    # Set architecture-specific paths
    if(ARCH_NAME STREQUAL "aarch64")
        set(MALI_DRIVER_PATH ${MALI_DRIVER_PATH_64})
        set(INSTALL_DIR "lib/aarch64-linux-gnu")
    elseif(ARCH_NAME STREQUAL "armhf")
        set(MALI_DRIVER_PATH ${MALI_DRIVER_PATH_32})
        set(INSTALL_DIR "lib/arm-linux-gnueabihf")
    else()
        message(FATAL_ERROR "Unknown architecture: ${ARCH_NAME}")
    endif()

    # Generate configuration header for this architecture
    configure_file(
        ${CMAKE_SOURCE_DIR}/cmake/config.hpp.in
        ${CMAKE_BINARY_DIR}/include/${ARCH_NAME}/config.hpp
        @ONLY
    )

    add_library(${TARGET_NAME} SHARED ${COMMON_SOURCES} ${WAYLAND_PROTOCOL_SOURCES} ${WSIALLOC_SOURCES} ${DRM_SOURCES})

    # Add dependency on wayland protocol generation
    add_dependencies(${TARGET_NAME} wayland_generated_files)
    if(BUILD_HUD)
        add_dependencies(${TARGET_NAME} hud_generated_files)
    endif()

    # Add architecture-specific include directory (for config.hpp)
    target_include_directories(${TARGET_NAME} PRIVATE
        ${COMMON_INCLUDES}
        ${CMAKE_BINARY_DIR}/include/${ARCH_NAME}
    )
    target_compile_definitions(${TARGET_NAME} PRIVATE ${COMMON_DEFINITIONS})
    target_link_libraries(${TARGET_NAME} PRIVATE ${COMMON_LIBRARIES})

    # Set architecture-specific flags
    if(ARCH_FLAGS)
        target_compile_options(${TARGET_NAME} PRIVATE ${ARCH_FLAGS})
        target_link_options(${TARGET_NAME} PRIVATE ${ARCH_FLAGS})
    endif()

    # Set output name
    set_target_properties(${TARGET_NAME} PROPERTIES
        OUTPUT_NAME "mali_wrapper"
        SOVERSION 1
        VERSION 1.0.0
    )

    if(BUILD_TESTING)
        file(GENERATE
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}_test_icd.json
            CONTENT "{\n  \"file_format_version\": \"1.0.0\",\n  \"ICD\": {\n    \"library_path\": \"$<TARGET_FILE:${TARGET_NAME}>\",\n    \"api_version\": \"${API_VERSION}\"\n  }\n}\n"
        )
    endif()

    # Install library to architecture-specific directory
    install(TARGETS ${TARGET_NAME}
        LIBRARY DESTINATION ${INSTALL_DIR}
        ARCHIVE DESTINATION ${INSTALL_DIR}
    )
endfunction()

# Detect current architecture (may be overridden by toolchain)
if(NOT DEFINED CURRENT_ARCH)
    execute_process(
        COMMAND uname -m
        OUTPUT_VARIABLE CURRENT_ARCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

# Build 64-bit wrapper
if(BUILD_64BIT)
    if(CURRENT_ARCH STREQUAL "aarch64")
        create_wrapper_target("aarch64" "")
        message(STATUS "Building 64-bit wrapper for aarch64")
    else()
        message(WARNING "64-bit build requested but not on aarch64 platform")
    endif()
endif()

# Create standalone target that builds single architecture (for cross-compilation)
if(NOT BUILD_32BIT AND NOT BUILD_64BIT)
    # If no specific build is requested, build for current architecture
    if(CURRENT_ARCH STREQUAL "aarch64")
        create_wrapper_target("aarch64" "")
        message(STATUS "Building standalone wrapper for aarch64")
    elseif(CURRENT_ARCH MATCHES "^arm")
        create_wrapper_target("armhf" "")
        message(STATUS "Building standalone wrapper for armhf")
    endif()
endif()

# Build 32-bit wrapper (requires cross-compilation on 64-bit systems)
if(BUILD_32BIT)
    if(CMAKE_CROSSCOMPILING)
        create_wrapper_target("armhf" "")
        message(STATUS "Building 32-bit wrapper using toolchain ${CMAKE_TOOLCHAIN_FILE}")
    elseif(CURRENT_ARCH MATCHES "^arm")
        create_wrapper_target("armhf" "")
        message(STATUS "Building 32-bit wrapper for armhf (native)")
    elseif(CURRENT_ARCH STREQUAL "aarch64")
        message(WARNING "32-bit build requested but cross-compilation toolchain not detected")
        message(STATUS "Install with: sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf")
    else()
        message(WARNING "32-bit build requested but not on ARM platform")
    endif()
endif()

# No configuration file needed - paths are compiled in

# Generate and install ICD manifests
if(INSTALL_ICDS)
    # 64-bit ICD manifest
    if(BUILD_64BIT)
        configure_file(
            ${CMAKE_SOURCE_DIR}/cmake/manifests/mali_icd.aarch64.json.in
            ${CMAKE_BINARY_DIR}/mali_icd.aarch64.json
            @ONLY
        )
        install(FILES ${CMAKE_BINARY_DIR}/mali_icd.aarch64.json
            DESTINATION share/vulkan/icd.d
        )
    endif()

    # 32-bit ICD manifest
    if(BUILD_32BIT)
        configure_file(
            ${CMAKE_SOURCE_DIR}/cmake/manifests/mali_icd.armhf.json.in
            ${CMAKE_BINARY_DIR}/mali_icd.armhf.json
            @ONLY
        )
        install(FILES ${CMAKE_BINARY_DIR}/mali_icd.armhf.json
            DESTINATION share/vulkan/icd.d
        )
    endif()
endif()

# Print build summary
message(STATUS "Mali Wrapper ICD Configuration:")
message(STATUS "  64-bit wrapper: ${BUILD_64BIT}")
message(STATUS "  32-bit wrapper: ${BUILD_32BIT}")
message(STATUS "  Install ICDs: ${INSTALL_ICDS}")
message(STATUS "  Integrated HUD: ${BUILD_HUD} (runtime opt-in)")
message(STATUS "  Current architecture: ${CURRENT_ARCH}")

if(BUILD_TESTING AND BUILD_HUD AND NOT CMAKE_CROSSCOMPILING)
    add_executable(mali_hud_tests
        tests/hud_tests.cpp
        src/hud/hud_config.cpp
        src/hud/hud_metadata.cpp
        src/hud/hud_metrics.cpp
    )
    target_include_directories(mali_hud_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${VULKAN_INCLUDE_DIRS}
    )
    target_compile_definitions(mali_hud_tests PRIVATE MALI_HUD_TEST_BUILD=1)
    target_link_libraries(mali_hud_tests PRIVATE pthread)
    add_test(NAME mali_hud_unit_tests COMMAND mali_hud_tests)
endif()
