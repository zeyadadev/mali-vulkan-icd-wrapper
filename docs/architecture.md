# Architecture

Mali Vulkan ICD Wrapper is installed as a Vulkan driver, not as an implicit
layer. The Vulkan loader opens an architecture-compatible wrapper library, and
that library opens the configured proprietary Mali driver.

## Build and installation

CMake creates one shared library per configured architecture:

```text
aarch64: lib/aarch64-linux-gnu/libmali_wrapper.so
armhf:   lib/arm-linux-gnueabihf/libmali_wrapper.so
```

It also creates the corresponding loader manifests:

```text
share/vulkan/icd.d/mali_icd.aarch64.json
share/vulkan/icd.d/mali_icd.armhf.json
```

Each build generates `config.hpp` with its architecture-specific
`MALI_DRIVER_PATH`. There is no runtime wrapper configuration file for choosing
the driver. Changing from the system g24 path to extracted g29p1 therefore
requires rebuilding that architecture.

Compatibility pNext structure sizes are generated at build time from the
installed Vulkan registry and headers. HUD builds also generate embedded SPIR-V
and font data.

## Loader and driver flow

At runtime:

1. The Vulkan loader discovers the installed ICD manifest that matches the
   application architecture.
2. It loads `libmali_wrapper.so` and negotiates the ICD entry points.
3. The wrapper opens the baked-in `libmali.so` with `dlopen`.
4. It resolves the Mali driver's `vk_icdGetInstanceProcAddr` and
   `vkCreateInstance`.
5. WSI, compatibility, memory-map, diagnostic, and HUD entry points stay in the
   wrapper.
6. Other Vulkan calls are obtained from and forwarded to the Mali driver.

The aarch64 and armhf wrappers follow the same path independently. This is what
lets a 64-bit game launcher and a 32-bit game use matching WSI and driver code
on the same installation.

## Integrated WSI

The proprietary driver is treated as not owning presentation. The wrapper
adds the required instance extensions, tracks Vulkan instances, physical
devices, logical devices, queues, and surfaces, and supplies:

- XCB and Xlib surfaces and swapchains;
- Wayland surfaces and swapchains;
- headless surfaces and swapchains.

Wayland uses DMA-BUF allocation and explicit synchronization support from the
integrated WSI implementation. X11 chooses DRI3, the legacy bridge, or SHM per
swapchain as described in [X11 presentation](x11-presentation.md).

## Compatibility boundary

The profile manager associates `VkApplicationInfo` with each instance and its
physical devices. Native applications and DXVK remain on the native profile.
vkd3d can receive bounded query and vertex-divisor adaptations, while unsafe
emulation is kept behind an environment variable.

Device creation is transformed on a copy of the application's structures.
Advertised emulated feature bits are removed before the request is sent to
Mali. Device-level hooks then implement the bounded or approximate behavior
owned by the wrapper.

See [Translation-layer compatibility](compatibility.md) for the exact policy.

## Memory mapping boundary

The wrapper tracks device-memory allocation sizes and intercepts map, unmap,
flush, invalidate, and queue submission calls needed by the optional
low-address workaround.

When enabled, a high Mali CPU mapping can become either:

- a second shared mapping of the same kbase pages through the patched low32
  alias ioctl; or
- a low shadow allocation synchronized with the original mapping.

All other mapping behavior remains with the driver. See
[Low-address mapping](low-address-mapping.md).

## HUD boundary

The HUD captures application metadata during instance creation and attaches to
wrapper-owned swapchains. It maintains its own Vulkan resources and metric
sampler. Failures disable the overlay for the process without changing the
application's presentation result.

This separation keeps the overlay optional at runtime even though its shaders
and font are compiled into a default build.
