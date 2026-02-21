# Mali Vulkan ICD Wrapper

A Vulkan ICD wrapper that solves the 32-bit/64-bit WSI layer compatibility problem for Mali GPUs. The Vulkan loader handles architecture routing, while each wrapper integrates the WSI layer and loads the correct Mali driver at runtime.

## Why This Exists

Mali's Vulkan drivers come in separate 32-bit and 64-bit flavors, and the Window System Integration (WSI) layer needs to match. The problem is that the Vulkan loader doesn't dynamically load WSI layers based on application architecture - even with multiple ICDs installed, only one architecture would work properly for WSI functions. This wrapper solves that by:

- **Integrated WSI**: Each wrapper has WSI layer functionality built-in
- **Runtime driver loading**: Loads the correct Mali driver at runtime based on build-time paths
- **Architecture routing**: Vulkan loader automatically selects the right wrapper (64-bit apps → 64-bit wrapper, 32-bit apps → 32-bit wrapper)
- **No config files**: Everything's baked in at build time
- **Drop-in replacement**: Just install and go

Built specifically for RK3588 SoCs with Mali G610 (g24p0) using libmali drivers from [tsukumijima/libmali-rockchip](https://github.com/tsukumijima/libmali-rockchip). Developed and tested on Armbian Ubuntu Noble with GNOME/Wayland.

## Quick Start

### Mali Driver Installation

**Important:** Install Mali drivers before building the wrapper to avoid conflicts.

#### 64-bit Mali Driver

Install the prebuilt Debian package:
```bash
# Download and install 64-bit Mali driver
wget https://github.com/ginkage/libmali-rockchip/releases/download/v1.9-1-04f8711/libmali-valhall-g610-g24p0-wayland-gbm_1.9-1_arm64.deb
sudo apt install ./libmali-valhall-g610-g24p0-wayland-gbm_1.9-1_arm64.deb
```

#### 32-bit Mali Driver

No prebuilt package exists, so download the binary directly:
```bash

# Download 32-bit Mali driver
sudo wget -O /usr/lib/arm-linux-gnueabihf/libmali-valhall-g610-g24p0-wayland-gbm.so \
  https://github.com/ginkage/libmali-rockchip/raw/refs/heads/master/lib/arm-linux-gnueabihf/libmali-valhall-g610-g24p0-wayland-gbm.so

# Create standard symlink (or keep original filename and use -DMALI_DRIVER_PATH_32 build option instead)
sudo ln -sf /usr/lib/arm-linux-gnueabihf/libmali-valhall-g610-g24p0-wayland-gbm.so \
           /usr/lib/arm-linux-gnueabihf/libmali.so
```

#### Remove Conflicting Mali ICD (CRITICAL)

**Most important:** Remove the default Mali ICD that gets installed with the driver package:

```bash
# Remove default Mali ICD (installed by .deb package) - THIS IS CRITICAL
sudo rm -f /usr/share/vulkan/icd.d/mali.json

# Also remove standalone WSI layer to avoid conflicts (if installed)
sudo rm -f /usr/share/vulkan/implicit_layer.d/VkLayer_window_system_integration.json
```

**Environment Variables:** Make sure no Vulkan environment variables are set (like `VK_ICD_FILENAMES`) unless you specifically want to override ICD selection.

### Prerequisites

**Note:** Complete the Mali Driver Installation above first, then install build dependencies.

```bash
# Add armhf architecture for 32-bit support
sudo dpkg --add-architecture armhf
sudo apt update

# Essential build tools
sudo apt install build-essential cmake pkg-config libvulkan-dev

# Window system support
sudo apt install libwayland-dev libx11-dev libx11-xcb-dev libdrm-dev
sudo apt install libxcb-shm0-dev libxcb-present-dev libxcb-sync-dev
sudo apt install libxrandr-dev wayland-protocols

# For 32-bit builds
sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf
sudo apt install libdrm-dev:armhf libwayland-dev:armhf libx11-dev:armhf
sudo apt install libx11-xcb-dev:armhf libxcb-shm0-dev:armhf
sudo apt install libxcb-present-dev:armhf libxcb-sync-dev:armhf libxrandr-dev:armhf
```

### Build and Install

**64-bit wrapper:**
```bash
cmake -S . -B build64 \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build64 -j$(nproc)
sudo cmake --install build64
```

**32-bit wrapper:**
```bash
cmake -S . -B build32 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/armhf_toolchain.cmake \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build32 -j$(nproc)
sudo cmake --install build32
```

**Both at once:**
```bash
# Configure both
cmake -S . -B build64 \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_BUILD_TYPE=Release
cmake -S . -B build32 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/armhf_toolchain.cmake \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_BUILD_TYPE=Release

# Build both
cmake --build build64 -j$(nproc) & cmake --build build32 -j$(nproc) & wait

# Install both
sudo cmake --install build64 && sudo cmake --install build32
```

### Test It

**64-bit testing (default tools):**
```bash
vulkaninfo --summary    # Should show Mali wrapper + Mali driver
vkcube                  # X11
vkcube-wayland          # Wayland
```

**32-bit testing (requires 32-bit tools):**
```bash
# Install 32-bit vulkan-tools
sudo apt install vulkan-tools:armhf

# Test 32-bit wrapper
vulkaninfo --summary
vkcube
```

**Verify wrapper installation:**
```bash
# Check ICD registration
ls -l /usr/share/vulkan/icd.d/mali_icd.*.json

# Verify library architecture
file /usr/lib/aarch64-linux-gnu/libmali_wrapper.so  # 64-bit
file /usr/lib/arm-linux-gnueabihf/libmali_wrapper.so   # 32-bit
```

## Configuration Options

| Option | What it does | Default |
|--------|--------------|---------|
| `BUILD_64BIT` | Build 64-bit wrapper (auto-detected from toolchain) | ON for native builds |
| `BUILD_32BIT` | Build 32-bit wrapper (auto-detected from toolchain) | ON for armhf cross-builds |
| `MALI_DRIVER_PATH_64` | Where your 64-bit Mali driver lives | `/usr/lib/aarch64-linux-gnu/libmali.so` |
| `MALI_DRIVER_PATH_32` | Where your 32-bit Mali driver lives | `/usr/lib/arm-linux-gnueabihf/libmali.so` |
| `BUILD_WSI_X11` | Enable X11 support | ON |
| `BUILD_WSI_WAYLAND` | Enable Wayland support | ON |
| `BUILD_WSI_HEADLESS` | Enable headless rendering | ON |
| `ENABLE_WAYLAND_FIFO_PRESENTATION_THREAD` | Use FIFO presentation thread | ON |
| `SELECT_EXTERNAL_ALLOCATOR` | External memory allocator backend | `dma_buf_heaps` |

## Experimental: X11 zero-copy via patched Xwayland

An experimental out-of-tree Xwayland dmabuf bridge patch flow is included in this repo.

- Patch series: `patches/xwayland/`
- Build script: `scripts/xwayland/build_patched_xwayland.sh`
- Full guide: `docs/xwayland-dmabuf-bridge.md`
- Runtime toggle: `XWL_DMABUF_BRIDGE=/run/user/$(id -u)/xwl-dmabuf.sock`
- Bridge modifier policy: non-linear modifiers are preferred by default; set `XWL_DMABUF_BRIDGE_PREFER_LINEAR=1` to force linear preference

## Debugging

The wrapper includes a configurable logging system. Set these environment variables:

```bash
# Log levels: 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG
export MALI_WRAPPER_LOG_LEVEL=2

# Categories: wrapper, wsi, wrapper+wsi
export MALI_WRAPPER_LOG_CATEGORY=wrapper+wsi

# Colors and console output
export MALI_WRAPPER_LOG_COLORS=1
export MALI_WRAPPER_LOG_CONSOLE=1

# Log to file
export MALI_WRAPPER_LOG_FILE=/tmp/mali_wrapper.log
```

## How It Works

1. **Build time**: CMake bakes Mali driver paths into each architecture-specific wrapper
2. **Install time**: ICD manifests get registered with the Vulkan loader pointing to each wrapper
3. **Runtime**: Vulkan loader picks the right wrapper based on app architecture
4. **Function calls**: Wrapper handles WSI functions with integrated WSI layer, forwards everything else to Mali driver
5. **Driver loading**: Wrapper loads the actual Mali driver at runtime using the baked-in path

No configuration files, no runtime detection, no architecture mismatches.

## Compatibility

**Tested Configuration:**
- **Hardware**: RK3588 SoCs with Mali G610
- **Kernel**: 6.1.115-vendor-rk35xx
- **OS**: Armbian Ubuntu Noble (24.04)
- **Desktop**: GNOME with Wayland
- **Mali Drivers**: libmali-rockchip v1.9-1 (g24p0)

## Credits

This wrapper integrates the [Vulkan WSI Layer](https://github.com/ginkage/vulkan-wsi-layer) by [ginkage](https://github.com/ginkage) to provide Window System Integration functionality. The WSI layer handles surface creation, swapchain management, and presentation for Wayland, X11, and headless rendering.
