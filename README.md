# Mali Vulkan ICD Wrapper

A Vulkan ICD wrapper that solves the 32-bit/64-bit WSI layer compatibility problem for Mali GPUs. The Vulkan loader handles architecture routing, while each wrapper integrates the WSI layer and loads the correct Mali driver at runtime.

## Why This Exists

Mali's Vulkan drivers come in separate 32-bit and 64-bit flavors, and the Window System Integration (WSI) layer needs to match. The problem is that the Vulkan loader doesn't dynamically load WSI layers based on application architecture - even with multiple ICDs installed, only one architecture would work properly for WSI functions. This wrapper solves that by:

- **Integrated WSI**: Each wrapper has WSI layer functionality built-in
- **Runtime driver loading**: Loads the correct Mali driver at runtime based on build-time paths
- **Architecture routing**: Vulkan loader automatically selects the right wrapper (64-bit apps → 64-bit wrapper, 32-bit apps → 32-bit wrapper)
- **No config files**: Everything's baked in at build time
- **Drop-in replacement**: Just install and go

Built specifically for RK3588 SoCs with Mali G610 using libmali drivers from [tsukumijima/libmali-rockchip](https://github.com/tsukumijima/libmali-rockchip). Supports g24p0 (system-installed for desktop acceleration) and g29p1 (extracted for Vulkan apps). Developed and tested on Armbian Ubuntu Noble with GNOME/Wayland.

## Quick Start

### Mali Driver Installation

**Important:** Install Mali drivers before building the wrapper to avoid conflicts.

Recommended (scripted):
```bash
./scripts/mali/install_mali_blobs.sh
```

By default, the script:
- **Installs the g24 blob** (`apt install`) — needed for desktop acceleration (system `libmali.so`)
- **Extracts the g29p1 blob** to `/opt/mali-g29p1` — newer driver for Vulkan apps, without replacing the system g24
- **Removes known conflicting Vulkan files** (`mali.json`, legacy WSI layer JSON)

Useful non-interactive examples:
```bash
# Install g24 + extract g29p1 (default behavior)
MALI_INSTALL_INTERACTIVE=0 ./scripts/mali/install_mali_blobs.sh

# Also install 32-bit Mali blob
MALI_INSTALL_INTERACTIVE=0 MALI_INSTALL_32BIT=1 ./scripts/mali/install_mali_blobs.sh

# Only install g24, skip g29p1 extraction
MALI_INSTALL_INTERACTIVE=0 MALI_EXTRACT_G29_64BIT=0 ./scripts/mali/install_mali_blobs.sh
```

#### Manual Mali Driver Installation

Use this only if you do not want the script.

```bash
# 64-bit g24 package (system install for desktop acceleration)
wget https://github.com/ginkage/libmali-rockchip/releases/download/v1.9-1-04f8711/libmali-valhall-g610-g24p0-wayland-gbm_1.9-1_arm64.deb
sudo apt install ./libmali-valhall-g610-g24p0-wayland-gbm_1.9-1_arm64.deb

# 64-bit g29p1 package (extract only for Vulkan apps)
wget https://github.com/ginkage/libmali-rockchip/releases/download/v1.9-1-4b399ed/libmali-valhall-g610-g29p1-x11-wayland-gbm_1.9-1_arm64.deb
sudo mkdir -p /opt/mali-g29p1
sudo dpkg-deb -x libmali-valhall-g610-g29p1-x11-wayland-gbm_1.9-1_arm64.deb /opt/mali-g29p1
sudo ln -sf /opt/mali-g29p1/usr/lib/aarch64-linux-gnu/libmali-valhall-g610-g29p1-*.so \
  /opt/mali-g29p1/usr/lib/aarch64-linux-gnu/libmali.so

# 32-bit blob + libmali.so symlink
sudo wget -O /usr/lib/arm-linux-gnueabihf/libmali-valhall-g610-g24p0-wayland-gbm.so \
  https://github.com/ginkage/libmali-rockchip/raw/refs/heads/master/lib/arm-linux-gnueabihf/libmali-valhall-g610-g24p0-wayland-gbm.so
sudo ln -sf /usr/lib/arm-linux-gnueabihf/libmali-valhall-g610-g24p0-wayland-gbm.so /usr/lib/arm-linux-gnueabihf/libmali.so

# Remove conflicting ICD/layer files
sudo rm -f /usr/share/vulkan/icd.d/mali.json
sudo rm -f /usr/share/vulkan/implicit_layer.d/VkLayer_window_system_integration.json
```

**Environment Variables:** Make sure no Vulkan environment variables are set (like `VK_ICD_FILENAMES`) unless you specifically want to override ICD selection.

### Prerequisites

Complete the Mali Driver Installation above first.

`scripts/wrapper/build_wrapper.sh` validates Mali driver paths before build:
- 64-bit: `/usr/lib/aarch64-linux-gnu/libmali.so` (system g24) or `/opt/mali-g29p1/usr/lib/aarch64-linux-gnu/libmali.so` (extracted g29p1)
- 32-bit: `/usr/lib/arm-linux-gnueabihf/libmali.so`
- override with `WRAPPER_MALI_DRIVER_PATH_64` / `WRAPPER_MALI_DRIVER_PATH_32` if your paths differ

In interactive mode, the build script auto-detects the extracted g29p1 blob and asks which driver to build against.

### Build and Install

Use the wrapper script:
```bash
./scripts/wrapper/build_wrapper.sh
```

On apt-based systems, answer `yes` when prompted for dependency installation.

Useful non-interactive examples:
```bash
# Build + install 64-bit only
WRAPPER_INTERACTIVE=0 WRAPPER_BUILD_64BIT=1 WRAPPER_BUILD_32BIT=0 ./scripts/wrapper/build_wrapper.sh

# Build + install both 64-bit and 32-bit
WRAPPER_INTERACTIVE=0 WRAPPER_BUILD_64BIT=1 WRAPPER_BUILD_32BIT=1 ./scripts/wrapper/build_wrapper.sh

# Also install apt build dependencies automatically
WRAPPER_INTERACTIVE=0 WRAPPER_INSTALL_BUILD_DEPS=1 WRAPPER_BUILD_64BIT=1 WRAPPER_BUILD_32BIT=1 ./scripts/wrapper/build_wrapper.sh

# Build/install 64-bit only and remove previous 32-bit wrapper artifacts
WRAPPER_INTERACTIVE=0 WRAPPER_BUILD_64BIT=1 WRAPPER_BUILD_32BIT=0 WRAPPER_PRUNE_UNSELECTED_ARCH=1 ./scripts/wrapper/build_wrapper.sh

# Disable automatic dma_heap udev setup
WRAPPER_INTERACTIVE=0 WRAPPER_CONFIGURE_DMA_HEAP_UDEV=0 ./scripts/wrapper/build_wrapper.sh
```

When `WRAPPER_INSTALL_SYSTEM=1` (default), the script automatically:
- checks/removes known conflicting Vulkan ICD files
- force-reinstalls selected wrapper outputs
- configures `/dev/dma_heap` udev permissions for non-root runtime
  - rule file: `/etc/udev/rules.d/99-mali-wrapper-dma-heap.rules`
  - default rule: `SUBSYSTEM=="dma_heap", OWNER="root", GROUP="video", MODE="0660"`
  - optional user group add (defaults to invoking user)

If the script adds your user to a new dma_heap access group, log out and back in once for group membership to take effect.

### Manual CMake

Only needed for custom CI/packaging flows. The script above is the recommended path.

```bash
# 64-bit
cmake -S . -B build64 -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release
cmake --build build64 -j$(nproc)
sudo cmake --install build64

# 32-bit
cmake -S . -B build32 -DCMAKE_TOOLCHAIN_FILE=cmake/armhf_toolchain.cmake -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release
cmake --build build32 -j$(nproc)
sudo cmake --install build32
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

### X11 Zero-Copy (Patched Xwayland)

For X11 apps running under Wayland, the wrapper includes a dmabuf bridge that enables zero-copy presentation through a patched Xwayland. This avoids the default SHM copy path.

```bash
./scripts/xwayland/build_patched_xwayland.sh
```

In interactive mode, the script prompts to install the patched binary to `/usr/bin/Xwayland` and configure the bridge socket environment. Log out and back in after install.

Full setup, runtime options, and rollback guide: [docs/xwayland-dmabuf-bridge.md](docs/xwayland-dmabuf-bridge.md)

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

### DXVK Compatibility Spoof (Experimental)

Some Mali blobs report required DXVK features as unsupported (for example `fillModeNonSolid`, `multiViewport`, `shaderClipDistance`, `shaderCullDistance`, and `robustBufferAccess2`), which causes adapter rejection.

You can opt-in to a wrapper spoof that advertises selected features to applications while masking them back off when creating the real device:

```bash
# Enable DXVK-oriented feature spoof bundle
export MALI_WRAPPER_FAKE_DXVK_FEATURES=1

# Or enable only fillModeNonSolid spoof explicitly
export MALI_WRAPPER_FAKE_FILL_MODE_NON_SOLID=1

# Or enable only multiViewport spoof explicitly
export MALI_WRAPPER_FAKE_MULTI_VIEWPORT=1

# Or enable only shader clip/cull distance spoof explicitly
export MALI_WRAPPER_FAKE_SHADER_CLIP_DISTANCE=1
export MALI_WRAPPER_FAKE_SHADER_CULL_DISTANCE=1

# Or enable only VK_EXT_robustness2 robustBufferAccess2 spoof explicitly
export MALI_WRAPPER_FAKE_ROBUST_BUFFER_ACCESS_2=1
```

Notes:
- This is a compatibility hack, not a hardware capability upgrade.
- If a game actually relies on spoofed features, behavior may still be incorrect.

Additional compatibility toggles:
- `MALI_WRAPPER_FILTER_EXTERNAL_MEMORY_HOST=1`: hide `VK_EXT_external_memory_host` from device extension enumeration and remove it from `vkCreateDevice` extension lists.
- `MALI_WRAPPER_LOW_ADDRESS_MAP=1`: explicitly enable low-address shadow mappings for `vkMapMemory`/`vkMapMemory2` to keep pointers 32-bit compatible. If unset, the workaround stays disabled.

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
- **Mali Drivers**: libmali-rockchip v1.9-1 (g24p0 for desktop, g29p1 for Vulkan apps)

## Credits

This wrapper integrates the [Vulkan WSI Layer](https://github.com/ginkage/vulkan-wsi-layer) by [ginkage](https://github.com/ginkage) to provide Window System Integration functionality. The WSI layer handles surface creation, swapchain management, and presentation for Wayland, X11, and headless rendering.
