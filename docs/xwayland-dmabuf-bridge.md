# Legacy Xwayland dmabuf bridge

The private dmabuf bridge is an out-of-tree Xwayland protocol for handing an
X11 swapchain image to Xwayland over a Unix `SOCK_SEQPACKET` socket. It predates
the wrapper's DRI3 presenter and is now kept as a fallback and debugging path.

Use the normal DRI3 path when it works. The bridge changes Xwayland, introduces
a private protocol, and has less runtime coverage.

## Repository contents

The Xwayland patch series currently contains:

| Patch | Purpose |
| --- | --- |
| `0001-xwayland-glamor-gbm-make-wl-drm-optional.patch` | Discover the main device without requiring `wl_drm` |
| `0002-xwayland-glamor-gbm-enable-mali-dmabuf-v3.patch` | Enable the Mali DMA-BUF v3 path |
| `0003-xwayland-glamor-gbm-skip-invalid-modifier-attrs.patch` | Avoid invalid modifier attributes |
| `0004-xwayland-glamor-finish-for-mali-sync.patch` | Complete glamor work for Mali synchronization |
| `0005-render-add-a4-picture-format.patch` | Add the A4 picture format needed by the target stack |
| `0006-xwayland-dmabuf-bridge-poc.patch` | Add the private socket and frame import path |
| `0007-xwayland-dmabuf-bridge-feedback-sync.patch` | Add bridge hello and feedback packets |
| `0008-xwayland-dmabuf-bridge-frame-callback-paced-feedback.patch` | Pace feedback with frame callbacks |
| `0009-xwayland-dmabuf-bridge-feedback-on-frame-callback.patch` | Finalize callback feedback behavior |
| `0010-xserver-meson-dri-fallback-to-xf86driproto.patch` | Add the Meson DRI protocol fallback |
| `0011-xwayland-dmabuf-bridge-glamor-guard.patch` | Guard bridge glamor integration |

The build helper is:

```text
scripts/xwayland/build_patched_xwayland.sh
```

It clones X.Org xserver, checks out `xwayland-23.2.6`, applies every
`patches/xwayland/*.patch` in lexical order, builds with glamor and DRI3, and
installs into:

```text
third_party/xserver/_install-bridge/bin/Xwayland
```

## Build

Run:

```bash
./scripts/xwayland/build_patched_xwayland.sh
```

On apt-based systems, the script can install its build dependencies:

```bash
XSERVER_INSTALL_BUILD_DEPS=1 \
./scripts/xwayland/build_patched_xwayland.sh
```

Important controls are:

| Variable | Default |
| --- | --- |
| `XSERVER_REPO` | `https://gitlab.freedesktop.org/xorg/xserver.git` |
| `XSERVER_TAG` | `xwayland-23.2.6` |
| `XSERVER_DIR` | `third_party/xserver` inside the repository |
| `BUILD_DIR` | `third_party/xserver/build-bridge` |
| `PREFIX_DIR` | `third_party/xserver/_install-bridge` |
| `XSERVER_SECURE_RPC` | `0` |
| `XSERVER_INSTALL_BUILD_DEPS` | `0` |
| `XSERVER_INSTALL_SYSTEM` | `0` |
| `XSERVER_REBOOT_AFTER_INSTALL` | `0` |
| `XSERVER_INTERACTIVE` | `auto` |

`MESON_EXTRA_ARGS` adds custom Meson arguments.

The helper always resets the selected `XSERVER_DIR` to the chosen tag and runs
`git clean -fd` before applying patches. Do not point it at an xserver checkout
with local work you need to keep.

## Install into the desktop session

The system install is opt-in:

```bash
XSERVER_INSTALL_SYSTEM=1 \
./scripts/xwayland/build_patched_xwayland.sh
```

By default this:

- copies the existing `/usr/bin/Xwayland` to `/usr/bin/Xwayland.orig` if that
  backup does not already exist;
- installs the patched binary as `/usr/bin/Xwayland`;
- writes `XWL_DMABUF_BRIDGE` to
  `~/.config/environment.d/90-xwl-bridge.conf`;
- leaves reboot disabled.

The default socket is:

```text
/run/user/<uid>/xwl-dmabuf.sock
```

Log out and back in after installation. Both the newly launched Xwayland
process and games started in that session need the same
`XWL_DMABUF_BRIDGE` value.

To install the patched binary without enabling the private bridge:

```bash
XSERVER_INSTALL_SYSTEM=1 \
XSERVER_CONFIGURE_BRIDGE_ENV=0 \
./scripts/xwayland/build_patched_xwayland.sh
```

That keeps the DRI3 fixes while leaving the bridge socket disabled.

## Force the bridge

When the environment is not persisted by the helper, export the socket path in
the session before Xwayland and the game start:

```bash
export XWL_DMABUF_BRIDGE=/run/user/"$(id -u)"/xwl-dmabuf.sock
```

The wrapper still prefers DRI3 unless the bridge is forced:

```bash
XWL_DMABUF_BRIDGE=/run/user/"$(id -u)"/xwl-dmabuf.sock \
WSI_X11_FORCE_BRIDGE=1 \
your-game
```

If the socket is missing or cannot be connected during swapchain creation, the
wrapper continues to DRI3 and then SHM.

## Bridge controls

| Variable | Default | Effect |
| --- | --- | --- |
| `XWL_DMABUF_BRIDGE_WAIT_FOR_FEEDBACK` | `0` | Wait for a frame acknowledgment when the server reports support |
| `XWL_DMABUF_BRIDGE_FEEDBACK_TIMEOUT_MS` | `250` | Feedback wait timeout, capped at 5000 ms |
| `XWL_DMABUF_BRIDGE_MAX_FPS` | Unset | Optional timer cap from 0 to 240 FPS |
| `XWL_DMABUF_BRIDGE_PREFER_LINEAR` | `0` | Prefer `DRM_FORMAT_MOD_LINEAR` when importable |
| `XWL_DMABUF_BRIDGE_RESERVED_FREE_IMAGES` | Automatic | Keep one image free, or two with `liblsfg-vk` loaded |
| `WSI_ALLOW_NON_FIFO_PRESENT_MODE` | `0` | Keep requested MAILBOX/IMMEDIATE mode instead of forcing FIFO |

Timer pacing is disabled when `XWL_DMABUF_BRIDGE_MAX_FPS` is unset or `0`.
Feedback is probed even in non-blocking mode; per-frame waits happen only when
explicitly enabled and supported by the patched server.

`XWL_DMABUF_BRIDGE_ALLOW_MAILBOX` remains a deprecated compatibility alias for
`WSI_ALLOW_NON_FIFO_PRESENT_MODE=1`.

## Troubleshooting

Enable detailed WSI logs:

```bash
MALI_WRAPPER_LOG_LEVEL=3 \
MALI_WRAPPER_LOG_CATEGORY=wsi \
MALI_WRAPPER_LOG_FILE=/tmp/mali-wrapper-bridge.log \
your-game
```

Useful checks:

```bash
test -S /run/user/"$(id -u)"/xwl-dmabuf.sock
journalctl -b --no-pager | grep -E 'xwayland dmabuf bridge|XWAYLAND:'
```

The wrapper log reports connection failure, feedback support, chosen formats
and modifiers, pacing, frame submission failures, and any switch away from the
bridge after a runtime failure.

Force SHM to confirm the failure is in a DMA-BUF path:

```bash
WSI_X11_FORCE_SHM=1 your-game
```

The general DRI3-first selection rules are in
[X11 presentation](x11-presentation.md).

## Roll back

If the helper created the default backup:

```bash
sudo install -m 0755 /usr/bin/Xwayland.orig /usr/bin/Xwayland
rm -f "$HOME/.config/environment.d/90-xwl-bridge.conf"
```

Log out and back in after restoring Xwayland. Remove or unset
`XWL_DMABUF_BRIDGE` from any other session configuration as well.
