# Mali Vulkan ICD Wrapper

Mali Vulkan ICD Wrapper is an experimental compatibility layer that makes
gaming with proprietary Mali G610 drivers more practical on RK3588 Linux
systems. It packages architecture-matched driver loading, X11 and Wayland
presentation, and a small set of Wine and vkd3d workarounds into a Vulkan ICD.

The wrapper does not replace the Mali driver or turn unsupported hardware
features into native ones. It sits between the Vulkan loader and `libmali.so`,
handles the WSI calls the proprietary driver does not provide, and forwards the
rest to the configured driver.

## Project status

This is a focused, experimental project for RK3588 devices. It changes pieces
of the graphics stack that normally stay independent, so updates to the Mali
blob, kernel, Xwayland, Wine, or a translation layer can change the result.
Keep a working desktop configuration and a way to undo system changes.

The current tested setup is:

| Component | Tested configuration |
| --- | --- |
| Hardware | RK3588 with Mali G610 |
| Kernel | `6.1.115-vendor-rk35xx` |
| Distribution | Armbian Ubuntu Noble 24.04 |
| Desktop | GNOME on Wayland |
| Mali userspace | g24p0 for the desktop; g29p1 for Vulkan applications |

Other systems may work, but they are not represented by the configuration
above.

## What it provides

- Separate aarch64 and armhf ICDs, each loading the matching Mali userspace
  driver.
- Integrated X11, Wayland, and headless WSI.
- A DRI3 + Present path for X11 applications under patched Xwayland, with SHM
  and a legacy private bridge as fallbacks.
- Safe, automatically selected vkd3d compatibility for specific driver gaps,
  plus an explicit unsafe mode for approximate feature emulation.
- Optional low-address mappings for 32-bit guests using a 64-bit Mali stack.
- An opt-in Vulkan HUD and standalone compatibility diagnostics.

DXVK deliberately sees the native Mali feature set. The wrapper's automatic
feature compatibility profile is limited to applications whose Vulkan engine
name is `vkd3d`.

## Quick start

These scripts target apt-based ARM64 systems and perform system-level changes.
Read [the installation guide](docs/installation.md) before using them on a
machine you cannot easily recover.

### 1. Install the Mali userspace drivers

```bash
./scripts/mali/install_mali_blobs.sh
```

The default run installs the 64-bit g24 package, extracts the older g29p1 v1.9
package under `/opt/mali-g29p1`, leaves 32-bit installation disabled, and
removes known conflicting Mali ICD and WSI layer files. Interactive mode asks
before proceeding and lets you choose the available g29p1 package version.

### 2. Build and install the wrapper

```bash
./scripts/wrapper/build_wrapper.sh
```

The build script defaults to a 64-bit system install. When it finds the
extracted g29p1 driver, interactive mode offers to build against it. To build
both architectures non-interactively:

```bash
WRAPPER_INTERACTIVE=0 \
WRAPPER_BUILD_64BIT=1 \
WRAPPER_BUILD_32BIT=1 \
WRAPPER_INSTALL_BUILD_DEPS=1 \
./scripts/wrapper/build_wrapper.sh
```

DMA-heap udev configuration is intentionally disabled by default. Enable it
only when the current `/dev/dma_heap` permissions prevent non-root use:

```bash
WRAPPER_CONFIGURE_DMA_HEAP_UDEV=1 ./scripts/wrapper/build_wrapper.sh
```

### 3. Verify the installation

Unset Vulkan loader overrides unless you are intentionally selecting an ICD,
then run:

```bash
vulkaninfo --summary
vkcube
vkcube-wayland
```

The installed manifests are:

```text
/usr/share/vulkan/icd.d/mali_icd.aarch64.json
/usr/share/vulkan/icd.d/mali_icd.armhf.json
```

Only the manifests for the architectures you built will be present.

## Optional HUD

The integrated HUD is built by default but stays off until requested:

```bash
MALI_HUD=1 vkcube
```

It reports frame timing, CPU and memory use, temperatures, GPU activity, the
Mali driver, and detected translation layers. It is designed to fail closed:
if HUD setup fails, the application continues without the overlay. See
[the HUD guide](docs/hud.md) for controls and build requirements.

## Documentation

- [Installation](docs/installation.md) — drivers, dependencies, scripted and
  manual builds, and verification.
- [Configuration](docs/configuration.md) — CMake settings and runtime
  environment variables.
- [Translation-layer compatibility](docs/compatibility.md) — native, safe
  vkd3d, and unsafe emulation behavior.
- [Low-address mapping](docs/low-address-mapping.md) — the shadow and patched
  kernel alias paths.
- [X11 presentation](docs/x11-presentation.md) — DRI3, SHM, present modes, and
  fallback selection.
- [HUD](docs/hud.md) — overlay contents, controls, and fault testing.
- [Debugging](docs/debugging.md) — logs, signal guards, and common failures.
- [Diagnostics](docs/diagnostics.md) — unit tests and the descriptor-buffer GPU
  diagnostic.
- [Architecture](docs/architecture.md) — how the loader, wrapper, WSI, and
  Mali driver fit together.
- [Legacy Xwayland dmabuf bridge](docs/xwayland-dmabuf-bridge.md) — detailed
  setup and rollback notes for the private bridge path.

## Important limitations

- Unsafe compatibility mode advertises approximate behavior. It can consume
  substantial memory, produce incorrect rendering, or expose driver crashes.
- The low-address shadow path copies mapped memory. The zero-copy alias path
  requires one of the included kernel patches and falls back to shadow memory
  if the alias ioctl is unavailable.
- The patched Xwayland and kernel helpers work in managed source trees and can
  install system components. Review their selected paths and prompts first.
- Proprietary Mali binaries are downloaded from external projects. They are
  not included in, or licensed by, this repository.
- Installed Vulkan overrides such as `VK_ICD_FILENAMES`, `VK_DRIVER_FILES`, or
  forced layers can make tests bypass the wrapper.

## Credits

The WSI implementation is derived from Arm's Vulkan WSI layer and integrates
work maintained by [GinKage](https://github.com/ginkage), including the
[Vulkan WSI layer](https://github.com/ginkage/vulkan-wsi-layer), patched
[Xwayland](https://github.com/ginkage/xserver), and
[libmali-rockchip](https://github.com/ginkage/libmali-rockchip) packaging.
The Xwayland patch series also retains its individual upstream author
attribution.

## License

Project-owned code is available under the [MIT License](LICENSE). Third-party
and derived source, fonts, kernel and Xwayland patches, and proprietary driver
packages retain their own notices and terms; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
