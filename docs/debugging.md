# Debugging

Start by proving which part of the stack is active. A useful first pass is:

```bash
vulkaninfo --summary
ls -l /usr/share/vulkan/icd.d/mali_icd.*.json
file /usr/lib/aarch64-linux-gnu/libmali_wrapper.so
```

For a 32-bit process, inspect the armhf wrapper instead. Also check whether
`VK_ICD_FILENAMES`, `VK_DRIVER_FILES`, `VK_LAYER_PATH`, or
`VK_INSTANCE_LAYERS` is selecting something other than the installed wrapper.

## Wrapper logs

The logger starts at error level, includes all categories, writes to the
console, and uses ANSI colors.

```bash
MALI_WRAPPER_LOG_LEVEL=2 \
MALI_WRAPPER_LOG_CATEGORY=wrapper+wsi \
MALI_WRAPPER_LOG_COLORS=0 \
MALI_WRAPPER_LOG_FILE=/tmp/mali-wrapper.log \
your-game
```

Levels are:

| Value | Level |
| --- | --- |
| `0` | Error |
| `1` | Warning |
| `2` | Info |
| `3` | Debug |

Categories are `wrapper`, `wsi`, and `low-address-map`. Join categories with
`+` or `,`. An invalid category disables logging, so copy the names exactly.

`MALI_WRAPPER_DEBUG` is an older shortcut: its presence forces debug level even
if its value is `0`. Prefer `MALI_WRAPPER_LOG_LEVEL=3` for explicit behavior.

## Driver loading failures

The path to `libmali.so` is compiled into each wrapper. An info log shows the
load attempt, while a missing library or missing
`vk_icdGetInstanceProcAddr` symbol is reported as an error.

If the configured path moved, rebuild with:

```bash
WRAPPER_MALI_DRIVER_PATH_64=/path/to/libmali.so \
./scripts/wrapper/build_wrapper.sh
```

For the default extracted g29p1 layout, the path is:

```text
/opt/mali-g29p1/usr/lib/aarch64-linux-gnu/libmali.so
```

## Presentation failures

Test Wayland and X11 separately:

```bash
vkcube-wayland
vkcube
```

For X11, enable WSI logs and compare the three presenters:

```bash
MALI_WRAPPER_LOG_LEVEL=3 MALI_WRAPPER_LOG_CATEGORY=wsi vkcube
WSI_X11_FORCE_SHM=1 vkcube
WSI_X11_DRI3_COPY=1 vkcube
```

If SHM succeeds while DRI3 fails, confirm the active Xwayland binary contains
the repository patch set. If a configured private bridge socket cannot be
reached, the wrapper should report that during swapchain creation and continue
to DRI3 or SHM.

See [X11 presentation](x11-presentation.md) for selection and present-mode
behavior.

## Translation-layer failures

At info level, instance creation prints the selected compatibility profile,
unsafe state, and engine name:

```bash
MALI_WRAPPER_LOG_LEVEL=2 your-game
```

DXVK should report the native profile. vkd3d should report the vkd3d profile
unless compatibility was disabled. Test unsafe mode only after confirming the
safe profile:

```bash
MALI_WRAPPER_COMPAT_PROFILE=vkd3d \
MALI_WRAPPER_UNSAFE_SPOOF=1 \
your-game
```

Return both variables to their defaults before comparing native applications.
The approximate paths and their risks are documented in
[Translation-layer compatibility](compatibility.md).

## Low-address mapping

Use level 1 for a live summary:

```bash
MALI_WRAPPER_LOW_ADDRESS_MAP=1 \
MALI_WRAPPER_LOW_ADDRESS_MAP_DEBUG=1 \
MALI_WRAPPER_LOG_LEVEL=2 \
MALI_WRAPPER_LOG_CATEGORY=low-address-map \
your-game
```

Use debug level 2 only when individual alias ioctls and copy events are needed.
It can produce a large log:

```bash
MALI_WRAPPER_LOW_ADDRESS_MAP=1 \
MALI_WRAPPER_LOW_ADDRESS_MAP_DEBUG=2 \
MALI_WRAPPER_LOG_LEVEL=3 \
MALI_WRAPPER_LOG_CATEGORY=low-address-map \
MALI_WRAPPER_LOG_FILE=/tmp/mali-wrapper-low-address.log \
your-game
```

An `alias` result uses the patched kernel path. A `shadow` result is functional
but copied. See [Low-address mapping](low-address-mapping.md).

## Signal diagnostics

Two opt-in tools exist for failures inside the proprietary driver:

```bash
MALI_WRAPPER_CRASH_SIGNAL_HANDLER=1 your-game
MALI_WRAPPER_GRAPHICS_PIPELINE_SIGNAL_GUARD=1 your-game
```

The crash handler prints a backtrace for fatal signals and then restores the
default handler so a core dump can still be produced. The graphics-pipeline
guard catches `SIGSEGV` or `SIGBUS` around Mali pipeline creation.

Both temporarily change process-wide signal handling. Wine, Box64, and games
may rely on their own handlers, so do not leave these enabled for normal use.

## Isolate the wrapper from the driver

The descriptor-buffer diagnostic can be run once with the wrapper ICD and once
with a direct Mali ICD manifest. A result reproduced in both paths points to
the underlying driver rather than wrapper dispatch. The exact build and run
commands are in [Diagnostics](diagnostics.md).
