# Integrated HUD

The wrapper can draw a compact performance overlay without a Vulkan layer or
`LD_PRELOAD`. HUD support is compiled by default and remains dormant until
enabled for a process:

```bash
MALI_HUD=1 your-game
```

HUD setup is isolated from the application. If its atlas, buffers, pipeline, or
metric sampler cannot be initialized, the application continues without the
overlay.

## What it shows

The panel includes:

- executable name, graphics API, and detected translation layer;
- resolution and display server;
- FPS and frame time;
- system and process CPU use;
- system and process memory use;
- CPU and GPU temperature;
- GPU load and clock;
- Mali driver and wrapper revisions.

DXVK, VKD3D-Proton, and Zink are detected from loaded modules and Vulkan
metadata. When available, the panel also identifies the DXVK client API and
translation-layer version. Sensors that cannot be read appear as `N/A`.

## Runtime controls

| Variable | Default | Values |
| --- | --- | --- |
| `MALI_HUD` | Disabled | `1`, `true`, `yes`, or `on` |
| `MALI_HUD_POSITION` | `top-left` | `top-left`, `top-right`, `bottom-left`, `bottom-right` |
| `MALI_HUD_SCALE` | `auto` | `auto` or `0.75` through `3.0` |
| `MALI_HUD_OPACITY` | `0.55` | `0` through `1` |
| `MALI_HUD_TEXT_OPACITY` | `0.90` | `0` through `1` |
| `MALI_HUD_INTERVAL_MS` | `500` | `100` through `5000` |
| `MALI_HUD_DEBUG` | Disabled | `1`, `true`, `yes`, or `on` |

For example:

```bash
MALI_HUD=1 \
MALI_HUD_POSITION=top-right \
MALI_HUD_SCALE=1.25 \
MALI_HUD_OPACITY=0.40 \
MALI_HUD_TEXT_OPACITY=0.85 \
your-game
```

Invalid values keep their defaults. Automatic scale follows display height,
and the layout shrinks when necessary so the panel does not exceed half of the
screen height.

## Build requirements

`BUILD_HUD=ON` requires:

- `glslangValidator` from `glslang-tools`;
- `stb/stb_truetype.h` from `libstb-dev`;
- `JetBrainsMono-Regular.ttf` from `fonts-jetbrains-mono`, or an explicit
  `HUD_FONT_FILE`;
- Python 3 for embedding generated assets.

On an apt-based system:

```bash
sudo apt install glslang-tools libstb-dev fonts-jetbrains-mono python3
```

Disable the HUD and its dependencies with:

```bash
cmake -S . -B build64 -DBUILD_HUD=OFF
```

The selected font is embedded into the built wrapper. Its OFL-1.1 text is in
`third_party/JetBrainsMono-OFL.txt` and is installed under
`share/doc/mali-wrapper` when the HUD is enabled.

## Debugging the HUD

Start with:

```bash
MALI_HUD=1 MALI_HUD_DEBUG=1 your-game
```

If the panel is absent, also enable wrapper logs and confirm that the process
loaded `libmali_wrapper.so`:

```bash
MALI_HUD=1 \
MALI_HUD_DEBUG=1 \
MALI_WRAPPER_LOG_LEVEL=2 \
your-game
```

Missing sensors alone are not a HUD initialization failure. They remain `N/A`
while frame and process metrics continue updating.

## Fault-injection build

Configure a native test build with fault hooks:

```bash
cmake -S . -B build64 \
  -DBUILD_TESTING=ON \
  -DMALI_HUD_TEST_BUILD=ON
cmake --build build64
```

`MALI_HUD_TEST_FAIL` can then inject `atlas`, `buffer`, `pipeline`, or `sensor`
failure:

```bash
MALI_HUD=1 MALI_HUD_TEST_FAIL=pipeline vkcube
```

The expected result is that the application keeps running without a broken
overlay. Unit-test commands are listed in [Diagnostics](diagnostics.md).
