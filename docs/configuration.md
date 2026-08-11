# Configuration

Most installations need no runtime configuration. Driver paths and build
features are compiled into each wrapper, while optional workarounds are
selected with environment variables at process startup.

Values below reflect the current CMake files and source code.

## CMake options

| Option | Default | Effect |
| --- | --- | --- |
| `BUILD_64BIT` | On for a native build | Build the aarch64 wrapper |
| `BUILD_32BIT` | On with `cmake/armhf_toolchain.cmake` | Build the armhf wrapper |
| `INSTALL_ICDS` | `ON` | Install architecture-specific ICD manifests |
| `BUILD_HUD` | `ON` | Compile the integrated, runtime-opt-in HUD |
| `MALI_HUD_TEST_BUILD` | `OFF` | Compile HUD fault-injection hooks |
| `BUILD_TESTING` | Normally `ON` through CTest | Build eligible test targets |
| `BUILD_WSI_X11` | `ON` | Compile XCB and Xlib WSI |
| `BUILD_WSI_WAYLAND` | `ON` | Compile Wayland WSI |
| `BUILD_WSI_HEADLESS` | `ON` | Compile headless WSI |
| `ENABLE_WAYLAND_FIFO_PRESENTATION_THREAD` | `ON` | Use the Wayland FIFO presentation thread |
| `SELECT_EXTERNAL_ALLOCATOR` | `dma_buf_heaps` | Select the WSI allocator backend |
| `WSIALLOC_MEMORY_HEAP_NAME` | `system-uncached` | Select the DMA-BUF heap |
| `MALI_DRIVER_PATH_64` | `/usr/lib/aarch64-linux-gnu/libmali.so` | Set the aarch64 driver loaded at runtime |
| `MALI_DRIVER_PATH_32` | `/usr/lib/arm-linux-gnueabihf/libmali.so` | Set the armhf driver loaded at runtime |
| `API_VERSION` | `1.3.276` | Set the API version in generated ICD manifests |
| `VULKAN_REGISTRY_XML` | `/usr/share/vulkan/registry/vk.xml` | Registry used for generated pNext metadata |
| `HUD_FONT_FILE` | Auto-detected JetBrains Mono Regular | Select the font embedded in HUD builds |

The architecture choice is forced by the selected toolchain: the normal
aarch64 configuration builds only 64-bit, while the included armhf toolchain
builds only 32-bit. Use separate build directories for a dual-architecture
installation.

## Runtime value conventions

The wrapper reads environment variables once or during object creation. Set
them on the same command that starts the game or launcher:

```bash
MALI_WRAPPER_LOG_LEVEL=2 MALI_HUD=1 vkcube
```

Boolean parsing is subsystem-specific. The examples use `0` and `1`, which are
accepted consistently.

## Core and logging controls

| Variable | Default | Effect |
| --- | --- | --- |
| `MALI_WRAPPER_DEBUG` | Unset | Presence forces the wrapper log level to debug |
| `MALI_WRAPPER_LOG_LEVEL` | `0` | `0` error, `1` warning, `2` info, `3` debug |
| `MALI_WRAPPER_LOG_CATEGORY` | All categories | `wrapper`, `wsi`, `low-address-map`, or a `+`/`,` combination |
| `MALI_WRAPPER_LOG_CONSOLE` | `1` | Set to `0` to suppress console output |
| `MALI_WRAPPER_LOG_COLORS` | `1` | Set to `0` to disable ANSI colors |
| `MALI_WRAPPER_LOG_FILE` | Unset | Append logs to the selected file |
| `MALI_WRAPPER_CRASH_SIGNAL_HANDLER` | `0` | Install an opt-in fatal-signal backtrace handler |
| `MALI_WRAPPER_GRAPHICS_PIPELINE_SIGNAL_GUARD` | `0` | Guard Mali graphics-pipeline creation against `SIGSEGV`/`SIGBUS` |

Both signal options replace process-wide signal handling while active. They can
interfere with Wine, Box64, or application crash handlers and should be used
only for diagnosis.

## Translation-layer compatibility

| Variable | Default | Effect |
| --- | --- | --- |
| `MALI_WRAPPER_COMPAT_PROFILE` | `auto` | `auto`, `off`, or force `vkd3d` |
| `MALI_WRAPPER_UNSAFE_SPOOF` | `0` | Enable approximate vkd3d-only feature emulation |
| `MALI_WRAPPER_SPARSE_COMMIT_BUDGET` | `8589934592` | Maximum dense sparse allocation bytes per device |
| `MALI_WRAPPER_FILTER_EXTERNAL_MEMORY_HOST` | Automatic for Wine WoW64 | Hide `VK_EXT_external_memory_host` during enumeration and device creation |
| `WINEWOW64` / `WINE_WOW64` | Unset | When present, enable external-memory-host filtering unless explicitly overridden |

An explicitly set `MALI_WRAPPER_FILTER_EXTERNAL_MEMORY_HOST=0` disables the
automatic Wine behavior. The sparse budget is read as an unsigned decimal byte
count and matters only in unsafe vkd3d mode.

See [Translation-layer compatibility](compatibility.md) before enabling unsafe
mode.

## Low-address mapping

| Variable | Default | Effect |
| --- | --- | --- |
| `MALI_WRAPPER_LOW_ADDRESS_MAP` | `0` | Return a sub-4-GiB alias or shadow pointer when Mali maps memory high |
| `MALI_WRAPPER_LOW_ADDRESS_MAP_DEBUG` | `0` | `1` enables progress and summary statistics; `2` adds event tracing |

The workaround first tries the patched-kernel alias ioctl and then falls back
to a copied shadow mapping. Full setup and behavior are in
[Low-address mapping](low-address-mapping.md).

## Presentation and X11 controls

| Variable | Default | Effect |
| --- | --- | --- |
| `WSI_X11_FORCE_SHM` | `0` | Force the X11 SHM copy presenter |
| `WSI_X11_FORCE_BRIDGE` | `0` | Prefer the configured legacy bridge over DRI3 |
| `WSI_X11_DRI3_COPY` | `0` | Add `XCB_PRESENT_OPTION_COPY` to DRI3 presents |
| `WSI_ALLOW_NON_FIFO_PRESENT_MODE` | `0` | Keep an application's non-FIFO mode on wrapper-owned Wayland, X11, or headless swapchains |
| `XWL_DMABUF_BRIDGE` | Unset | Unix socket path for the legacy private bridge |
| `XWL_DMABUF_BRIDGE_WAIT_FOR_FEEDBACK` | `0` | Wait for synchronous bridge feedback when supported |
| `XWL_DMABUF_BRIDGE_FEEDBACK_TIMEOUT_MS` | `250` | Feedback timeout, capped at 5000 ms |
| `XWL_DMABUF_BRIDGE_MAX_FPS` | Unset | Optional bridge timer cap from 0 to 240 FPS; unset or `0` disables it |
| `XWL_DMABUF_BRIDGE_PREFER_LINEAR` | `0` | Prefer a linear modifier on the bridge path |
| `XWL_DMABUF_BRIDGE_RESERVED_FREE_IMAGES` | Automatic | Keep one image free, or two when `liblsfg-vk` is loaded |
| `XWL_DMABUF_BRIDGE_ALLOW_MAILBOX` | `0` | Deprecated alias for `WSI_ALLOW_NON_FIFO_PRESENT_MODE=1` |
| `WSI_DISPLAY_DRI_DEV` | `/dev/dri/card0` | DRM node for builds that include direct display WSI |

`WSI_X11_FORCE_BRIDGE=1` still needs a reachable `XWL_DMABUF_BRIDGE` socket.
When it is unavailable, selection continues to DRI3 and then SHM.

The wrapper forces every non-FIFO request to FIFO by default before selecting
the Wayland, X11, or headless presentation implementation. This also ensures
that Wayland swapchains use the FIFO presentation thread when that build option
is enabled. Set `WSI_ALLOW_NON_FIFO_PRESENT_MODE=1` only when explicitly opting
in to application-selected MAILBOX, IMMEDIATE, or FIFO_RELAXED behavior.

See [X11 presentation](x11-presentation.md) and the
[legacy bridge guide](xwayland-dmabuf-bridge.md) for the selection order and
patched Xwayland setup.

## HUD controls

| Variable | Default | Accepted values |
| --- | --- | --- |
| `MALI_HUD` | `0` | `1`, `true`, `yes`, or `on` |
| `MALI_HUD_POSITION` | `top-left` | `top-left`, `top-right`, `bottom-left`, `bottom-right` |
| `MALI_HUD_SCALE` | `auto` | `auto` or `0.75` through `3.0` |
| `MALI_HUD_OPACITY` | `0.55` | `0` through `1` |
| `MALI_HUD_TEXT_OPACITY` | `0.90` | `0` through `1` |
| `MALI_HUD_INTERVAL_MS` | `500` | `100` through `5000` |
| `MALI_HUD_DEBUG` | `0` | `1`, `true`, `yes`, or `on` |
| `MALI_HUD_TEST_FAIL` | Unset | `atlas`, `buffer`, `pipeline`, or `sensor` in a test build |

Invalid HUD values leave the corresponding default unchanged. See
[HUD](hud.md) for build requirements and displayed metrics.
