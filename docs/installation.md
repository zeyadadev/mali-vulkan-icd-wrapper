# Installation

The recommended installation uses the repository scripts. They know the paths
expected by the CMake build, keep the desktop g24 driver separate from the
application-facing g29p1 driver, and install matching Vulkan ICD manifests.

Both scripts can change system files. Run them from the repository root and
read their summary before accepting an interactive install.

## Before you begin

The supported workflow assumes:

- an aarch64 RK3588 system;
- an apt-based distribution;
- `sudo` or an existing root shell for system installation;
- a working Mali kernel driver and `/dev/mali0`;
- network access for the driver installer.

Do not set `VK_ICD_FILENAMES`, `VK_LAYER_PATH`, or `VK_INSTANCE_LAYERS` during
initial verification unless the override is intentional. The build script
warns when it finds these variables.

## Install the Mali blobs

Run:

```bash
./scripts/mali/install_mali_blobs.sh
```

The defaults come directly from `scripts/mali/install_mali_blobs.sh`:

| Setting | Default behavior |
| --- | --- |
| `MALI_INSTALL_INTERACTIVE` | `auto`; prompt when stdin and stdout are terminals |
| `MALI_INSTALL_64BIT` | Install the g24p0 v1.9 ARM64 package |
| `MALI_EXTRACT_G29_64BIT` | Extract a g29p1 package to `/opt/mali-g29p1` |
| `MALI_G29_VERSION` | `old`, the g29p1 v1.9 package |
| `MALI_INSTALL_32BIT` | Disabled |
| `MALI_REMOVE_CONFLICTING_ICD` | Remove known conflicting Mali ICD/layer JSON files |
| `MALI_G29_CLEAN_EXTRACT` | Clean the selected extraction directory first |
| `MALI_DOWNLOAD_DIR` | `/tmp/mali-wrapper-blobs` |

The `latest` g29p1 choice selects the `v1.10-1-334a20c` package currently
encoded in the script. A custom package URL can be supplied through
`MALI_G29_64_DEB_URL`.

Useful unattended runs:

```bash
# Keep the defaults: g24 install, old g29p1 extraction, no 32-bit blob.
MALI_INSTALL_INTERACTIVE=0 ./scripts/mali/install_mali_blobs.sh

# Select the newer g29p1 package known to the script.
MALI_INSTALL_INTERACTIVE=0 \
MALI_G29_VERSION=latest \
./scripts/mali/install_mali_blobs.sh

# Install the 32-bit blob as well.
MALI_INSTALL_INTERACTIVE=0 \
MALI_INSTALL_32BIT=1 \
./scripts/mali/install_mali_blobs.sh

# Install only g24 and skip g29p1 extraction.
MALI_INSTALL_INTERACTIVE=0 \
MALI_EXTRACT_G29_64BIT=0 \
./scripts/mali/install_mali_blobs.sh
```

The installer puts the desktop-facing g24 driver at the distribution's normal
aarch64 library path. It extracts g29p1 without installing it over g24, then
creates regular `libmali.so` loader copies under:

```text
/opt/mali-g29p1/usr/lib/aarch64-linux-gnu/
```

The optional armhf blob is installed at:

```text
/usr/lib/arm-linux-gnueabihf/libmali-valhall-g610-g24p0-wayland-gbm.so
```

with `/usr/lib/arm-linux-gnueabihf/libmali.so` pointing to it.

### Manual blob setup

The current scripted sources can also be installed manually. This example uses
the script's default g24 and older g29p1 packages:

```bash
wget https://github.com/ginkage/libmali-rockchip/releases/download/v1.9-1-04f8711/libmali-valhall-g610-g24p0-wayland-gbm_1.9-1_arm64.deb
sudo apt install ./libmali-valhall-g610-g24p0-wayland-gbm_1.9-1_arm64.deb

wget https://github.com/ginkage/libmali-rockchip/releases/download/v1.9-1-4b399ed/libmali-valhall-g610-g29p1-x11-wayland-gbm_1.9-1_arm64.deb
sudo mkdir -p /opt/mali-g29p1
sudo dpkg-deb -x \
  libmali-valhall-g610-g29p1-x11-wayland-gbm_1.9-1_arm64.deb \
  /opt/mali-g29p1
```

The installer performs more cleanup and alias creation than these commands.
Use its final summary to reproduce the exact `libmali.so` layout when doing the
work by hand.

Known conflicting files are:

```text
/usr/share/vulkan/icd.d/mali.json
/etc/vulkan/icd.d/mali.json
/usr/share/vulkan/implicit_layer.d/VkLayer_window_system_integration.json
/etc/vulkan/implicit_layer.d/VkLayer_window_system_integration.json
```

Review them before manual removal if they belong to another package you still
need.

## Build and install the wrapper

Run:

```bash
./scripts/wrapper/build_wrapper.sh
```

The script starts with a 64-bit Release build and system installation enabled.
In interactive mode it detects
`/opt/mali-g29p1/usr/lib/aarch64-linux-gnu/libmali.so` and offers to use it
instead of the system g24 driver.

Important script controls:

| Variable | Default | Purpose |
| --- | --- | --- |
| `WRAPPER_INTERACTIVE` | `auto` | Enable prompts when attached to a terminal |
| `WRAPPER_BUILD_64BIT` | `1` | Build the aarch64 ICD |
| `WRAPPER_BUILD_32BIT` | `0` | Build the armhf ICD |
| `WRAPPER_INSTALL_BUILD_DEPS` | `0` | Install apt build dependencies |
| `WRAPPER_INSTALL_SYSTEM` | `1` | Install libraries and manifests |
| `WRAPPER_CLEAN` | `0` | Recreate selected build directories |
| `WRAPPER_PRUNE_UNSELECTED_ARCH` | `0` | Remove an installed architecture not selected now |
| `WRAPPER_BUILD_TYPE` | `Release` | Set `CMAKE_BUILD_TYPE` |
| `WRAPPER_INSTALL_PREFIX` | `/usr` | Set the CMake install prefix |
| `WRAPPER_JOBS` | `nproc` | Parallel build jobs |
| `WRAPPER_MALI_DRIVER_PATH_64` | `/usr/lib/aarch64-linux-gnu/libmali.so` | Baked-in aarch64 driver path |
| `WRAPPER_MALI_DRIVER_PATH_32` | `/usr/lib/arm-linux-gnueabihf/libmali.so` | Baked-in armhf driver path |
| `WRAPPER_CONFIGURE_DMA_HEAP_UDEV` | `0` | Install a DMA-heap permission rule |
| `WRAPPER_REBOOT_AFTER_INSTALL` | `0` | Reboot after a successful install |

Examples:

```bash
# Build and install the 64-bit wrapper against extracted g29p1.
WRAPPER_INTERACTIVE=0 \
WRAPPER_MALI_DRIVER_PATH_64=/opt/mali-g29p1/usr/lib/aarch64-linux-gnu/libmali.so \
./scripts/wrapper/build_wrapper.sh

# Build and install both architectures, including apt dependencies.
WRAPPER_INTERACTIVE=0 \
WRAPPER_BUILD_64BIT=1 \
WRAPPER_BUILD_32BIT=1 \
WRAPPER_INSTALL_BUILD_DEPS=1 \
./scripts/wrapper/build_wrapper.sh

# Build without installing.
WRAPPER_INTERACTIVE=0 \
WRAPPER_INSTALL_SYSTEM=0 \
./scripts/wrapper/build_wrapper.sh
```

When installation is enabled, the script checks for conflicts, removes known
conflicting files, clears the selected wrapper artifacts, installs the fresh
outputs, and compares installed libraries with the build products. It does not
prune the unselected architecture unless explicitly requested.

### DMA-heap access

DMA-heap udev setup is off by default. If applications fail because the user
cannot access `/dev/dma_heap/*`, enable it:

```bash
WRAPPER_CONFIGURE_DMA_HEAP_UDEV=1 ./scripts/wrapper/build_wrapper.sh
```

The default rule is written to
`/etc/udev/rules.d/99-mali-wrapper-dma-heap.rules` and assigns DMA-heap devices
to `root:video` with mode `0660`. The script may add the invoking user to the
`video` group; log out and back in after that change.

## Manual CMake build

The HUD is enabled by default, so a normal build needs
`glslangValidator`, stb headers, JetBrains Mono, and Python in addition to the
Vulkan, Wayland, DRM, X11, and XCB development packages.

Native aarch64 build:

```bash
cmake -S . -B build64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DMALI_DRIVER_PATH_64=/opt/mali-g29p1/usr/lib/aarch64-linux-gnu/libmali.so
cmake --build build64 -j"$(nproc)"
sudo cmake --install build64
```

armhf cross-build:

```bash
cmake -S . -B build32 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/armhf_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DMALI_DRIVER_PATH_32=/usr/lib/arm-linux-gnueabihf/libmali.so
cmake --build build32 -j"$(nproc)"
sudo cmake --install build32
```

The manifests contain `/usr` library paths. A different install prefix
therefore also requires packaging or adjusting the installed manifests.

See [Configuration](configuration.md) for all project CMake options.

## Verify the result

For a 64-bit installation:

```bash
vulkaninfo --summary
vkcube
vkcube-wayland
file /usr/lib/aarch64-linux-gnu/libmali_wrapper.so
```

For armhf, install suitable 32-bit Vulkan tools and check:

```bash
sudo apt install vulkan-tools:armhf
file /usr/lib/arm-linux-gnueabihf/libmali_wrapper.so
```

List the registered ICDs with:

```bash
ls -l /usr/share/vulkan/icd.d/mali_icd.*.json
```

If the wrong driver appears, first inspect Vulkan loader overrides and then
confirm which `libmali.so` path was baked into the selected build directory.
The [debugging guide](debugging.md) covers the next checks.
