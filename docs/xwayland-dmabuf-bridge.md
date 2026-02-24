# Xwayland dmabuf bridge (experimental)

This repository carries an out-of-tree Xwayland patch series for a single-window X11 zero-copy path over Wayland.

## What is in-repo

- Patch series:
  - `patches/xwayland/0001-xwayland-dmabuf-bridge-poc.patch`
  - `patches/xwayland/0002-xwayland-dmabuf-bridge-feedback-sync.patch`
  - `patches/xwayland/0003-xwayland-dmabuf-bridge-frame-callback-paced-feedback.patch`
  - `patches/xwayland/0004-xwayland-dmabuf-bridge-feedback-on-frame-callback.patch`
- Build helper:
  - `scripts/xwayland/build_patched_xwayland.sh`

Patch summary:

- `0001`: initial dmabuf bridge socket + packet ABI + import/commit path.
- `0002`: bridge sync protocol (`HELLO` / `FEEDBACK`) for ACK-based pacing in wrapper.
- `0003`: gate success feedback using frame callback pacing behavior.
- `0004`: finalize success feedback semantics on `wl_surface.frame` callback (present cadence).

The script clones upstream `xserver`, checks out `xwayland-23.2.6`, applies local patches, builds, and installs into:

- `third_party/xserver/_install-bridge/bin/Xwayland`

Note: the script runs `git reset --hard` and `git clean -fd` in `XSERVER_DIR`.

## Prerequisites

```bash
sudo apt update
sudo apt install git meson ninja-build ccache pkg-config
sudo apt install libxkbfile-dev libxshmfence-dev libxfont-dev libfontenc-dev libxcvt-dev
# optional; if installed, you may set XSERVER_SECURE_RPC=true
sudo apt install libtirpc-dev
```

You can also let the build script install these dependencies automatically on apt-based systems:

```bash
XSERVER_INSTALL_BUILD_DEPS=1 ./scripts/xwayland/build_patched_xwayland.sh
```

## Build patched Xwayland

```bash
./scripts/xwayland/build_patched_xwayland.sh
```

By default, when run in a terminal, the script is interactive and prompts for:

- dependency install
- system install to `/usr/bin/Xwayland`
- persistent bridge env setup (`~/.config/environment.d/90-xwl-bridge.conf`)
- reboot after install

Control this with:

```bash
# force interactive prompts
XSERVER_INTERACTIVE=1 ./scripts/xwayland/build_patched_xwayland.sh

# force non-interactive behavior (use env flags only)
XSERVER_INTERACTIVE=0 ./scripts/xwayland/build_patched_xwayland.sh
```

Useful overrides:

```bash
# choose a different checkout dir
XSERVER_DIR=/tmp/xserver ./scripts/xwayland/build_patched_xwayland.sh

# pass extra Meson options
MESON_EXTRA_ARGS='-Dglamor=true -Dxwayland_ei=false' ./scripts/xwayland/build_patched_xwayland.sh

# if libtirpc-dev is installed and desired
XSERVER_SECURE_RPC=true ./scripts/xwayland/build_patched_xwayland.sh

# auto-install build deps (apt only)
XSERVER_INSTALL_BUILD_DEPS=1 ./scripts/xwayland/build_patched_xwayland.sh

# install patched binary into /usr/bin/Xwayland (backs up to /usr/bin/Xwayland.orig once)
XSERVER_INSTALL_SYSTEM=1 ./scripts/xwayland/build_patched_xwayland.sh

# install system-wide and reboot automatically at the end
XSERVER_INSTALL_SYSTEM=1 XSERVER_REBOOT_AFTER_INSTALL=1 ./scripts/xwayland/build_patched_xwayland.sh

# override persistent bridge env behavior/path when system install is enabled
XSERVER_INSTALL_SYSTEM=1 XSERVER_CONFIGURE_BRIDGE_ENV=0 ./scripts/xwayland/build_patched_xwayland.sh
XSERVER_INSTALL_SYSTEM=1 XSERVER_BRIDGE_SOCKET_PATH=/run/user/$(id -u)/xwl-dmabuf.sock ./scripts/xwayland/build_patched_xwayland.sh
XSERVER_INSTALL_SYSTEM=1 XSERVER_BRIDGE_ENV_FILE="$HOME/.config/environment.d/90-xwl-bridge.conf" ./scripts/xwayland/build_patched_xwayland.sh
```

System install/reboot options are always opt-in:

- `XSERVER_INSTALL_SYSTEM=1`: install `${PREFIX_DIR}/bin/Xwayland` to `/usr/bin/Xwayland`
- `XSERVER_SYSTEM_XWAYLAND_PATH=/path/to/Xwayland`: override install destination
- `XSERVER_SYSTEM_XWAYLAND_BACKUP_PATH=/path/to/backup`: override backup path
- `XSERVER_CONFIGURE_BRIDGE_ENV=1` (default with system install): write persistent `XWL_DMABUF_BRIDGE` env file
- `XSERVER_BRIDGE_SOCKET_PATH=/run/user/<uid>/xwl-dmabuf.sock`: socket path written to env file
- `XSERVER_BRIDGE_ENV_FILE=~/.config/environment.d/90-xwl-bridge.conf`: persistent env file path
- `XSERVER_REBOOT_AFTER_INSTALL=1`: reboot after build/install

## Use patched Xwayland in session

GNOME/mutter launches `/usr/bin/Xwayland` directly, so install/rollback is typically:

```bash
sudo cp /usr/bin/Xwayland /usr/bin/Xwayland.orig
sudo install -m 0755 third_party/xserver/_install-bridge/bin/Xwayland /usr/bin/Xwayland
```

Log out and log back in.

To roll back:

```bash
sudo install -m 0755 /usr/bin/Xwayland.orig /usr/bin/Xwayland
```

## Bridge runtime configuration

The patch adds an optional UNIX socket bridge enabled by:

```bash
export XWL_DMABUF_BRIDGE=/run/user/$(id -u)/xwl-dmabuf.sock
```

When `XSERVER_INSTALL_SYSTEM=1`, the helper script now configures this automatically by default via:

- `~/.config/environment.d/90-xwl-bridge.conf`

You still need to log out and log back in so the next Xwayland process inherits it.

The socket ABI and packet format are documented in:

- `third_party/xserver/hw/xwayland/DMABUF_BRIDGE.md` (generated when patch is applied)

## Wrapper integration status

Implemented in this repo:

1. X11 swapchain bridge client (`AF_UNIX`, `SOCK_SEQPACKET`) that sends packet + dmabuf FDs via `sendmsg(..., SCM_RIGHTS, ...)`.
2. X11 swapchain dmabuf image allocation/binding path for bridge mode.
3. Legacy SDL helper-window routing is now opt-in only (`WSI_FORCE_SDL_WAYLAND=1`).
4. Existing SHM presenter path remains available when `XWL_DMABUF_BRIDGE` is unset.

Runtime behavior:

- `XWL_DMABUF_BRIDGE` set: use Xwayland dmabuf bridge path for X11 swapchains.
- `XWL_DMABUF_BRIDGE_PREFER_LINEAR=1`: prefer `DRM_FORMAT_MOD_LINEAR` (default behavior now prefers non-linear modifiers when available).
- `XWL_DMABUF_BRIDGE_MAX_FPS=<N>`: cap bridge present rate (`0` disables pacing, default is `60` in bridge mode when no override is set).
- `XWL_DMABUF_BRIDGE_FEEDBACK_TIMEOUT_MS=<N>`: timeout for ACK-based frame feedback (default `250` ms) before falling back to timer pacing.
- `WSI_ALLOW_NON_FIFO_PRESENT_MODE=1`: keep the app-selected present mode (MAILBOX/IMMEDIATE/etc.) for layer-owned X11 and Wayland swapchains (default behavior forces FIFO for compatibility).
- `XWL_DMABUF_BRIDGE_ALLOW_MAILBOX=1`: legacy alias for bridge-specific setups (deprecated; prefer `WSI_ALLOW_NON_FIFO_PRESENT_MODE=1`).
- `XWL_DMABUF_BRIDGE` unset: use existing SHM presenter path.
- `WSI_FORCE_SDL_WAYLAND=1`: force legacy SDL workaround path (for fallback testing only).

## Troubleshooting

- Bridge socket present but no frames:
  - `journalctl -b --no-pager | grep -E "xwayland dmabuf bridge|XWAYLAND:"`
- Import failures now log non-fatal diagnostics from patched Xwayland, for example:
  - `xwayland dmabuf bridge: compositor rejected dmabuf xid=... format=... modifier=... planes=... p0_offset=... p0_stride=...`
  - `xwayland dmabuf bridge: failed to commit created buffer xid=... format=... modifier=...`
- The bridge now uses `zwp_linux_buffer_params_v1_create()` (non-fatal import path), so a bad frame should not terminate Xwayland.
- With the updated Xwayland patch series, the bridge includes ACK-based feedback (`HELLO`/`FEEDBACK`) and the wrapper enforces timer-capped pacing in bridge mode.
  - ACK success is emitted on `wl_surface.frame` callback (present cadence signal).
  - Wrapper log when active: `Xwayland bridge: sync feedback active; enforcing timer cap as an additional bridge safety bound.`
  - Wrapper fallback log (old Xwayland): `Xwayland bridge: sync feedback unsupported by server, using fallback pacing`
- If explicit linear import fails (`modifier=0x0`), patched Xwayland retries once with implicit modifier semantics (`DRM_FORMAT_MOD_INVALID`) and logs:
  - `xwayland dmabuf bridge: retrying linear import with implicit modifier ...`
