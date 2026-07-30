# Third-party notices

The root [MIT License](LICENSE) applies to code and documentation owned by the
mali-vulkan-icd-wrapper contributors. This repository also contains derived
source, license material, and patches whose existing notices and upstream terms
remain in effect.

This file is a practical index, not a replacement for the notices in individual
files or the licenses of the projects those files modify.

## Vulkan WSI source

Much of `src/wsi` is derived from Arm's Vulkan WSI layer and from integration
work maintained by GinKage. Those files retain Arm copyright notices and
`SPDX-License-Identifier: MIT` markers where supplied.

Relevant upstream projects include:

- <https://github.com/ARM-software/vulkan-wsi-layer>
- <https://github.com/ginkage/vulkan-wsi-layer>

Keep the file-level notices when copying or redistributing this source.

## JetBrains Mono

HUD builds embed a locally installed `JetBrainsMono-Regular.ttf`. JetBrains
Mono is licensed under the SIL Open Font License 1.1.

The repository's copy of that license is:

- [`third_party/JetBrainsMono-OFL.txt`](third_party/JetBrainsMono-OFL.txt)

The font binary is discovered from the build system or selected through
`HUD_FONT_FILE`; it is not stored in this repository.

## X.Org and Xwayland patches

Files under `patches/xwayland` modify the X.Org xserver/Xwayland source tree.
The series retains available git-format author and origin metadata, including
changes authored or adapted from work by Simon Ser, Kevin Li, Robin Wang, Kai
Guo, and mali-vulkan-icd-wrapper contributors.

The helper script obtains its target source from:

- <https://gitlab.freedesktop.org/xorg/xserver>

Applying the patches does not replace the copyright notices or license terms
of the affected xserver files. Review the target source's licenses when
building or distributing a patched Xwayland binary. Third-party patches in the
series are not implicitly relicensed by the root project license.

## Linux and Mali kbase patches

Files under `patches/kernel` modify Linux and Arm Mali kbase driver source. The
patch headers retain their author metadata, and the patch bodies contain
context from the target upstream files.

The default helper workflow targets:

- <https://github.com/zeyadadev/linux-rockchip>

Use and distribution of a patched kernel remain subject to the licenses and
notices of the Linux and Mali driver source being modified. The repository's
MIT license does not replace those upstream terms.

## Proprietary Mali userspace drivers

The repository does not distribute the proprietary Mali driver packages used
at runtime. `scripts/mali/install_mali_blobs.sh` downloads or extracts external
packages from the libmali-rockchip project:

- <https://github.com/ginkage/libmali-rockchip>

Those binaries are not covered by this repository's MIT license. Obtain and use
them only under the terms supplied by their provider and any applicable Arm
license.
