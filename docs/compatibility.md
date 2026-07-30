# Translation-layer compatibility

The compatibility subsystem is deliberately narrow. Native Vulkan
applications and DXVK see the Mali driver's real feature set. A separate vkd3d
profile covers a few known gaps without changing capabilities for every
application on the system.

This is compatibility logic, not hardware feature implementation. Safe mode
intercepts operations the wrapper can answer in a bounded way. Unsafe mode
uses approximations and must be enabled explicitly.

## Profile selection

The wrapper records a profile when `vkCreateInstance` succeeds.

| Setting | Selection |
| --- | --- |
| `MALI_WRAPPER_COMPAT_PROFILE=auto` or unset | Select vkd3d only when `VkApplicationInfo::pEngineName` equals `vkd3d`, case-insensitively |
| `MALI_WRAPPER_COMPAT_PROFILE=off` | Keep native behavior |
| `MALI_WRAPPER_COMPAT_PROFILE=vkd3d` | Force the vkd3d profile |

DXVK is intentionally treated as native. Shader and pipeline fallbacks for
DXVK belong in the DXVK build rather than in global ICD feature advertising.

The active profile is written to the wrapper log at info level:

```bash
MALI_WRAPPER_LOG_LEVEL=2 your-game
```

## Safe vkd3d behavior

The automatically selected vkd3d profile provides two bounded adaptations.

### Pipeline-statistics queries

The wrapper advertises `pipelineStatisticsQuery` to vkd3d, intercepts
pipeline-statistics query pools, and returns initialized zero counters.
Availability values are reported as ready. Query commands and result copies
are handled by the wrapper instead of being passed to a driver feature that
was not enabled.

This keeps applications that require the query interface running, but the
numbers are placeholders and must not be used for performance analysis.

### Vertex attribute divisor bridge

When the Mali driver exposes `VK_KHR_vertex_attribute_divisor`, the wrapper
also exposes the corresponding `VK_EXT_vertex_attribute_divisor` interface to
vkd3d. EXT extension requests and structures are translated to their KHR
counterparts. The maximum divisor is read from the driver's native KHR
properties.

Ordinary, non-zero divisors use the driver's real implementation. A zero
divisor is not advertised in safe mode.

## Unsafe vkd3d mode

Enable the approximate paths only for a title that cannot get past feature
checks in safe mode:

```bash
MALI_WRAPPER_UNSAFE_SPOOF=1 your-game
```

Unsafe mode applies only while the vkd3d profile is active. It adds or adjusts
the following claims:

- zero vertex attribute divisors;
- `robustBufferAccess2` and `robustImageAccess2`;
- single-texel storage-buffer alignment with a one-byte alignment value;
- vertex-stage stores and atomics;
- sparse binding, buffer residency, single-sample 2D image residency, and
  sparse aliasing;
- shader resource residency and minimum LOD.

The wrapper removes emulated feature enables before calling `vkCreateDevice`,
so unsupported bits are not sent to the Mali driver.

These claims are approximated as follows:

- zero vertex divisors are rewritten to `UINT32_MAX` during graphics pipeline
  creation;
- sparse buffers and supported sparse images are created as ordinary,
  fully-allocated resources;
- `vkQueueBindSparse` becomes a semaphore-only queue submission because the
  resource is already resident;
- robustness and texel-alignment claims do not add hardware behavior.

Dense sparse image emulation is limited to single-sample 2D images. Sparse
buffers and images consume their full memory up front, so a workload designed
around partial residency can use far more memory than expected.

## Dense sparse budget

The wrapper limits dense sparse allocations to 8 GiB per Vulkan device by
default. Set a decimal byte value to change that limit:

```bash
MALI_WRAPPER_SPARSE_COMMIT_BUDGET=4294967296 your-game
```

An allocation that would exceed the remaining budget fails with an
out-of-device-memory result. Invalid values fall back to 8 GiB.

## External-memory-host filtering

Wine WoW64 processes may expose `WINEWOW64` or `WINE_WOW64`. When either is
present, the wrapper hides `VK_EXT_external_memory_host` from device extension
enumeration and removes it from device creation requests.

The behavior can be controlled directly:

```bash
# Force filtering.
MALI_WRAPPER_FILTER_EXTERNAL_MEMORY_HOST=1 your-game

# Keep the extension even when a Wine WoW64 variable is present.
MALI_WRAPPER_FILTER_EXTERNAL_MEMORY_HOST=0 your-game
```

This filter is independent of the vkd3d safe and unsafe profiles.

## Choosing a mode

Start with the default `auto` profile and unsafe mode disabled. If a vkd3d
application fails, collect an info-level log and confirm that `profile=vkd3d`
appears. Force the profile only when the engine name is missing or changed by
the application.

Unsafe mode is a last compatibility step. Rendering errors, unexpectedly high
memory use, and driver crashes are possible because advertised behavior does
not exactly match hardware behavior. Disable it before diagnosing unrelated
native or DXVK problems.

The unit coverage for profile selection, feature overlays, device-create
sanitization, query results, divisor rewriting, and dense sparse submission is
described in [Diagnostics](diagnostics.md).
