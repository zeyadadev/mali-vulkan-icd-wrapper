# Diagnostics

The repository has native unit tests for compatibility policy and HUD helpers,
plus a Vulkan GPU diagnostic for descriptor buffers. Test targets are not
built during armhf cross-compilation.

## Configure a test build

For a normal build with every available test:

```bash
cmake -S . -B build64 -DBUILD_TESTING=ON
cmake --build build64 -j"$(nproc)"
ctest --test-dir build64 --output-on-failure
```

The registered native tests are:

- `mali_compatibility_unit_tests`;
- `mali_hud_unit_tests` when `BUILD_HUD=ON`.

The compatibility tests cover profile detection, native feature preservation,
safe and unsafe feature policy, the EXT-to-KHR divisor bridge, device-create
sanitization, bounded query results, zero-divisor rewriting, and dense sparse
resource submission.

If HUD dependencies are unavailable and only compatibility tests are needed:

```bash
cmake -S . -B build64 \
  -DBUILD_TESTING=ON \
  -DBUILD_HUD=OFF
cmake --build build64 --target mali_compatibility_tests
./build64/mali_compatibility_tests
```

## Descriptor-buffer GPU diagnostic

When testing is enabled and `glslangValidator` is found, CMake creates
`mali_descriptor_buffer_gpu_test`. It compares descriptor sets with descriptor
buffers for:

- storage buffers;
- sampled images;
- mutable descriptors;
- fixed descriptor arrays;
- variable-count descriptor arrays.

Build and run it against the installed aarch64 wrapper:

```bash
cmake -S . -B build64 -DBUILD_TESTING=ON
cmake --build build64 --target mali_descriptor_buffer_gpu_test

VK_LOADER_LAYERS_DISABLE='~implicit~' \
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/mali_icd.aarch64.json \
./build64/mali_descriptor_buffer_gpu_test
```

The program prints the selected device and driver version, skips unsupported
extension cases, and reports each descriptor-set and descriptor-buffer result.

## Current g29p1 finding

On the proprietary g29p1 driver used by this project, fixed-count
descriptor-buffer layouts pass. Layouts combining
`VK_EXT_descriptor_buffer` with
`VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT` cause shader descriptor
reads to resolve to zero.

The same result was observed through the wrapper and through a direct Mali ICD
path, which places this behavior below wrapper dispatch. Repeat both runs after
changing the Mali blob: one with `mali_icd.aarch64.json`, and one with the
direct driver manifest available on that system.

Do not leave `VK_DRIVER_FILES` set globally after the test; it overrides normal
ICD discovery.

## Reading failures

- A configure-time warning about `glslangValidator` means the GPU diagnostic
  target was not created.
- A missing wrapper manifest means the selected architecture was not installed.
- An extension skip means the selected driver did not advertise the feature
  required by that case.
- A wrapper-only difference should be retested with wrapper debug logging and
  implicit layers disabled before assigning it to dispatch behavior.
