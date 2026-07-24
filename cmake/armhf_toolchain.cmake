# Cross-compilation toolchain for the integrated Mali wrapper + WSI layer (armhf)
# This file is used for building the 32-bit artifacts from a 64-bit host.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_CROSSCOMPILING TRUE)

# Toolchain
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
if(DEFINED ENV{MALI_ARMHF_CXX_COMPILER})
    set(CMAKE_CXX_COMPILER "$ENV{MALI_ARMHF_CXX_COMPILER}")
else()
    set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
endif()

# Target sysroot and search paths
set(_ARMHF_SYSROOT /usr/arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH ${_ARMHF_SYSROOT} /usr/lib/arm-linux-gnueabihf /usr/lib/gcc-cross/arm-linux-gnueabihf/13 /usr)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Compiler flags tuned for Cortex-A75 class devices (Mali G610 platforms)
set(CMAKE_C_FLAGS_INIT "-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -Wno-psabi")
set(CMAKE_CXX_FLAGS_INIT "-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -Wno-psabi")
set(CMAKE_EXE_LINKER_FLAGS_INIT "")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "")

# pkg-config setup for cross builds
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${_ARMHF_SYSROOT}")

# Wayland protocols are consumed from the host installation
set(WAYLAND_PROTOCOLS_DIR "/usr/share/wayland-protocols" CACHE PATH "Wayland protocols directory" FORCE)

# Build only the 32-bit wrapper when this toolchain is active
set(BUILD_32BIT ON CACHE BOOL "Build 32-bit wrapper" FORCE)
set(BUILD_64BIT OFF CACHE BOOL "Build 64-bit wrapper" FORCE)
