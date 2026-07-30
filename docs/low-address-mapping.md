# Low-address mapping

Some 32-bit guest runtimes need a pointer returned by `vkMapMemory` to fit
below 4 GiB even when the Vulkan process and Mali userspace driver are 64-bit.
The wrapper can provide a low pointer without changing applications:

```bash
MALI_WRAPPER_LOW_ADDRESS_MAP=1 your-game
```

The workaround is off by default. When it is enabled, pointers that already
fit in 32 bits are returned unchanged.

## How a high mapping is handled

For a Mali mapping above 4 GiB, the wrapper tries two paths in order:

1. Ask a patched kbase driver to create a second CPU mapping of the same pages,
   then map that alias below 4 GiB.
2. Allocate a low shadow region and copy between it and the real Mali mapping.

The alias path is shared and zero-copy. It keeps the driver's original high
mapping alive while returning the low alias to the application.

The shadow path works without a patched kernel, but it has a real cost. The
wrapper copies initial contents into the shadow, copies flushed ranges back to
Mali memory, refreshes invalidated ranges, and performs additional
synchronization before submissions and cleanup.

If neither path can produce a 32-bit-compatible pointer, the original high
pointer remains visible and the log records the failure when diagnostics are
enabled.

## Kernel patches

The repository includes two kbase patch variants:

- `patches/kernel/0001-mali-add-valhall-low32-alias-mapping-support.patch`
  for the Valhall tree used by the current g29p1 kernel configuration;
- `patches/kernel/0002-mali-add-bifrost-low32-alias-mapping-support.patch`
  for older or differently configured trees.

Both add a low32 alias ioctl to the target Mali kernel driver. The wrapper
discovers open `/dev/mali0` file descriptors, calls that ioctl for the real CPU
mapping, and maps the returned cookie with `MAP_SHARED`.

A stock kernel simply rejects or does not recognize the ioctl. The wrapper
then falls back to shadow memory.

## Build the patched kernel

The helper script targets the repository's tested linux-rockchip workflow:

```bash
./scripts/kernel/build_patched_linux_rockchip.sh
```

Its main defaults are:

| Variable | Default |
| --- | --- |
| `KERNEL_REPO` | `https://github.com/zeyadadev/linux-rockchip.git` |
| `KERNEL_BRANCH` | `rk-6.1-rkr6.1` |
| `KERNEL_DIR` | `third_party/linux-rockchip` inside this repository |
| `KERNEL_MALI_DRIVER` | `valhall` |
| `KERNEL_ARCH` | `arm64` |
| `KERNEL_LOCALVERSION` | `-low32alias` |
| `KERNEL_INSTALL_BUILD_DEPS` | `0` |
| `KERNEL_APPLY_PATCH` | `1` |
| `KERNEL_INSTALL_PACKAGES` | `0` |
| `KERNEL_REBOOT_AFTER_INSTALL` | `0` |

The script seeds `.config` from `KERNEL_CONFIG_SOURCE` or the running kernel
when possible, applies the selected git-format patch, configures the matching
Mali driver, and builds Debian packages with `make bindeb-pkg`.

To build the Bifrost variant:

```bash
KERNEL_MALI_DRIVER=bifrost \
./scripts/kernel/build_patched_linux_rockchip.sh
```

To install the packages produced by the script:

```bash
KERNEL_INSTALL_PACKAGES=1 \
./scripts/kernel/build_patched_linux_rockchip.sh
```

Package installation and reboot remain separate choices by default.

## Source-tree safety

The default kernel directory is managed by the helper and may be reset and
cleaned. When `KERNEL_DIR` points to an existing external checkout, the script
leaves its remotes alone and defaults away from resetting it unless
`KERNEL_RESET_TREE=1` is explicitly supplied.

Still review the selected directory before continuing. A kernel build also
uses much more disk space than a wrapper build, especially with
`bindeb-pkg`.

Install the matching kernel image, device trees, and modules together. Booting
a new image with modules from another build can prevent the Mali driver from
loading.

The patches modify an upstream kernel/Mali driver tree and are not relicensed
by the wrapper's project license. See
[Third-party notices](../THIRD_PARTY_NOTICES.md).

## Confirm the selected path

Enable progress statistics:

```bash
MALI_WRAPPER_LOW_ADDRESS_MAP=1 \
MALI_WRAPPER_LOW_ADDRESS_MAP_DEBUG=1 \
MALI_WRAPPER_LOG_LEVEL=2 \
MALI_WRAPPER_LOG_CATEGORY=low-address-map \
your-game
```

The shutdown summary distinguishes mappings that were already low from
`alias` and `shadow` mappings.

For individual ioctl, mapping, copy, and cleanup events, use debug level 2:

```bash
MALI_WRAPPER_LOW_ADDRESS_MAP=1 \
MALI_WRAPPER_LOW_ADDRESS_MAP_DEBUG=2 \
MALI_WRAPPER_LOG_LEVEL=3 \
MALI_WRAPPER_LOG_CATEGORY=low-address-map \
MALI_WRAPPER_LOG_FILE=/tmp/mali-wrapper-low-address.log \
your-game
```

An alias result confirms that the patched ioctl was available for that
mapping. A shadow result means compatibility is active but copying is still
required.
