# X11 presentation

The wrapper supplies X11 WSI for Mali applications through XCB and Xlib. On a
Wayland desktop, X11 games normally run through Xwayland, so the selected
presentation path depends on both the wrapper and the Xwayland build.

There are three paths:

- DRI3 + Present imports swapchain DMA-BUFs as X pixmaps and is the preferred
  path.
- The private Xwayland dmabuf bridge passes DMA-BUFs over a Unix socket and is
  retained as a legacy fallback.
- SHM copies the image into an X11 shared-memory image and is the universal
  fallback.

## Automatic selection

Each X11 swapchain chooses its presenter in this order:

1. `WSI_X11_FORCE_SHM=1` selects SHM.
2. `WSI_X11_FORCE_BRIDGE=1` selects the bridge when
   `XWL_DMABUF_BRIDGE` is configured and reachable.
3. A working DRI3 and Present connection selects DRI3.
4. A reachable bridge socket selects the bridge.
5. Otherwise the swapchain uses SHM.

The bridge is probed during swapchain creation. A missing socket does not defer
the failure until the first present; the wrapper immediately continues to DRI3
or SHM.

Use info-level WSI logs to see the selected path:

```bash
MALI_WRAPPER_LOG_LEVEL=2 \
MALI_WRAPPER_LOG_CATEGORY=wsi \
your-game
```

## DRI3 + Present

DRI3 is the normal zero-copy X11 path. The wrapper allocates an importable
DMA-BUF, creates an X pixmap through DRI3, and queues it through the X Present
extension. The current patch set carries the Xwayland glamor and Mali DMA-BUF
fixes needed by the tested stack.

Build the patched Xwayland tree with:

```bash
./scripts/xwayland/build_patched_xwayland.sh
```

The helper defaults to Xwayland 23.2.6, uses
`third_party/xserver` as its managed checkout, applies every patch under
`patches/xwayland`, and builds into a repository-local prefix. A system install
is disabled by default.

The script can install its result over `/usr/bin/Xwayland`, preserving the
original once as `/usr/bin/Xwayland.orig`:

```bash
XSERVER_INSTALL_SYSTEM=1 \
./scripts/xwayland/build_patched_xwayland.sh
```

This affects every X11 application in the Wayland session. Finish testing the
local build and keep a non-graphical recovery path before replacing the system
binary.

For a copy-style DRI3 present rather than the default path:

```bash
WSI_X11_DRI3_COPY=1 your-game
```

## Present modes

The wrapper forces requested MAILBOX, IMMEDIATE, or FIFO_RELAXED presentation
back to FIFO by default on every wrapper-owned swapchain. On X11, FIFO uses
Present MSC pacing and is the stable choice on the patched Xwayland/Mali
combination.

Keep the application's non-FIFO request with:

```bash
WSI_ALLOW_NON_FIFO_PRESENT_MODE=1 your-game
```

This is an opt-in tradeoff and can flicker. The older
`XWL_DMABUF_BRIDGE_ALLOW_MAILBOX` variable remains a deprecated compatibility
alias.

## Legacy bridge

The private bridge remains useful for debugging or when DRI3 cannot be
initialized. It needs a patched Xwayland process listening on the socket
provided through `XWL_DMABUF_BRIDGE`.

Force it with:

```bash
XWL_DMABUF_BRIDGE=/run/user/"$(id -u)"/xwl-dmabuf.sock \
WSI_X11_FORCE_BRIDGE=1 \
your-game
```

Bridge feedback waits, pacing, modifier preference, image reservation, session
setup, and rollback are documented in the
[legacy bridge guide](xwayland-dmabuf-bridge.md).

## SHM fallback

SHM is slower because pixels are copied into an X11 shared-memory image, but it
does not depend on Xwayland DMA-BUF import. Force it when separating a WSI
problem from a driver or game problem:

```bash
WSI_X11_FORCE_SHM=1 your-game
```

If SHM works while DRI3 does not, collect WSI debug logs and verify that the
running Xwayland binary is the patched one. If both fail, confirm the wrapper
ICD is selected and test Wayland presentation separately with
`vkcube-wayland`.

All X11 environment variables and defaults are listed in
[Configuration](configuration.md).
