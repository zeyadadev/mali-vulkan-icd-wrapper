#include "mali_wrapper_icd.hpp"
#include "library_loader.hpp"
#include "wsi_manager.hpp"
#include "wsi/wsi_private_data.hpp"
#include "wsi/wsi_factory.hpp"
#include "wsi/layer_utils/extension_list.hpp"
#include <vulkan/vk_icd.h>
#include "config.hpp"
#include "../utils/logging.hpp"
#include <cstring>
#include <unordered_set>
#include <unordered_map>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <cinttypes>
#include <dlfcn.h>
#include <cstdio>
#include <memory>
#include <string>
#include <chrono>
#include <mutex>
#include <atomic>
#include <limits>
#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <fcntl.h>
#include <execinfo.h>
#include <signal.h>
#include <setjmp.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace mali_wrapper {

struct InstanceInfo {
    VkInstance instance;
    int ref_count;
    std::chrono::steady_clock::time_point destroy_time;
    bool marked_for_destruction;

    InstanceInfo(VkInstance inst) : instance(inst), ref_count(1), marked_for_destruction(false) {}
};

static std::unordered_map<VkInstance, std::unique_ptr<InstanceInfo>> managed_instances;
static std::unordered_map<VkDevice, VkInstance> managed_devices;
static std::mutex instance_mutex;
static VkInstance latest_instance = VK_NULL_HANDLE;

struct DeviceMemoryKey {
    VkDevice device;
    VkDeviceMemory memory;

    bool operator==(const DeviceMemoryKey& other) const {
        return device == other.device && memory == other.memory;
    }
};

struct DeviceMemoryKeyHash {
    size_t operator()(const DeviceMemoryKey& key) const noexcept {
        const auto device_hash = std::hash<void*>{}(reinterpret_cast<void*>(key.device));
        const auto memory_hash = std::hash<void*>{}(reinterpret_cast<void*>(key.memory));
        return device_hash ^ (memory_hash << 1);
    }
};

struct ShadowMappingInfo {
    void* real_ptr = nullptr;
    void* shadow_ptr = nullptr;
    size_t shadow_size = 0;
    VkDeviceSize offset = 0;
    VkDeviceSize mapped_size = 0;
};

enum class ShadowAllocationMethod {
    NONE = 0,
    MAP_32BIT = 1,
    FIXED_SEARCH = 2,
};

enum class LowAddressCopyKind {
    MAP_TO_SHADOW = 0,
    FLUSH_TO_REAL = 1,
    INVALIDATE_TO_SHADOW = 2,
    SUBMIT_TO_REAL = 3,
    UNMAP_TO_REAL = 4,
};

struct ShadowAllocationResult {
    void* ptr = nullptr;
    size_t size = 0;
    ShadowAllocationMethod method = ShadowAllocationMethod::NONE;
    uint32_t fixed_search_attempts = 0;
    int last_errno = 0;
    uint64_t elapsed_ns = 0;
};

struct LowAddressMapStats {
    std::atomic<uint64_t> successful_maps{0};
    std::atomic<uint64_t> compatible_maps{0};
    std::atomic<uint64_t> high_pointer_maps{0};
    std::atomic<uint64_t> shadow_maps{0};
    std::atomic<uint64_t> disabled_high_pointer_maps{0};
    std::atomic<uint64_t> resolve_failures{0};
    std::atomic<uint64_t> unsupported_size_failures{0};
    std::atomic<uint64_t> allocation_failures{0};
    std::atomic<uint64_t> map32bit_allocations{0};
    std::atomic<uint64_t> fixed_search_allocations{0};
    std::atomic<uint64_t> fixed_search_attempts{0};
    std::atomic<uint64_t> active_shadow_maps{0};
    std::atomic<uint64_t> peak_shadow_maps{0};
    std::atomic<uint64_t> active_shadow_bytes{0};
    std::atomic<uint64_t> peak_shadow_bytes{0};
    std::atomic<uint64_t> total_shadow_bytes_reserved{0};
    std::atomic<uint64_t> initial_copy_bytes{0};
    std::atomic<uint64_t> flush_copy_bytes{0};
    std::atomic<uint64_t> invalidate_copy_bytes{0};
    std::atomic<uint64_t> submit_copy_bytes{0};
    std::atomic<uint64_t> unmap_copy_bytes{0};
    std::atomic<uint64_t> allocation_time_ns{0};
    std::atomic<uint64_t> copy_time_ns{0};
};

struct LowAddressMapReportSnapshot {
    uint64_t successful_maps = 0;
    uint64_t compatible_maps = 0;
    uint64_t high_pointer_maps = 0;
    uint64_t shadow_maps = 0;
    uint64_t disabled_high_pointer_maps = 0;
    uint64_t allocation_failures = 0;
    uint64_t active_shadow_maps = 0;
    uint64_t active_shadow_bytes = 0;
    uint64_t total_copy_bytes = 0;
};

static std::mutex memory_tracking_mutex;
static std::unordered_map<DeviceMemoryKey, VkDeviceSize, DeviceMemoryKeyHash> tracked_memory_allocations;
static std::unordered_map<DeviceMemoryKey, ShadowMappingInfo, DeviceMemoryKeyHash> shadow_mappings;
static LowAddressMapStats low_address_map_stats;
static std::mutex low_address_map_report_mutex;
static bool low_address_map_report_initialized = false;
static LowAddressMapReportSnapshot low_address_map_last_report_snapshot;
static std::chrono::steady_clock::time_point low_address_map_last_report_time;
static std::mutex graphics_pipeline_signal_guard_mutex;
static thread_local sigjmp_buf graphics_pipeline_signal_guard_env;
static thread_local volatile sig_atomic_t graphics_pipeline_signal_guard_active = 0;
static thread_local volatile sig_atomic_t graphics_pipeline_signal_guard_caught_signal = 0;

static constexpr uint64_t kMax32BitAddressExclusive = 0x100000000ULL;
static constexpr uintptr_t kShadowSearchStart = 0x10000000ULL;
static constexpr uintptr_t kShadowSearchEnd = 0xF0000000ULL;
static constexpr uintptr_t kShadowSearchStep = 0x00100000ULL;

static DeviceMemoryKey make_memory_key(VkDevice device, VkDeviceMemory memory)
{
    return DeviceMemoryKey{ device, memory };
}

static bool is_bool_env_enabled(const char* name, bool default_value)
{
    const char* value = getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return default_value;
    }

    if (value[0] == '0' || value[0] == 'n' || value[0] == 'N' ||
        value[0] == 'f' || value[0] == 'F') {
        return false;
    }
    return true;
}

static int get_low_address_map_debug_level()
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }

    const char* value = getenv("MALI_WRAPPER_LOW_ADDRESS_MAP_DEBUG");
    if (value == nullptr || value[0] == '\0') {
        cached = 0;
        return cached;
    }

    if (value[0] >= '0' && value[0] <= '9') {
        cached = std::max(0, std::atoi(value));
        return cached;
    }

    cached = is_bool_env_enabled("MALI_WRAPPER_LOW_ADDRESS_MAP_DEBUG", false) ? 1 : 0;
    return cached;
}

static bool should_use_low_address_shadow_map()
{
    static int cached = -1;
    if (cached >= 0) {
        return cached == 1;
    }

    cached = is_bool_env_enabled("MALI_WRAPPER_LOW_ADDRESS_MAP", false) ? 1 : 0;
    return cached == 1;
}

static bool should_collect_low_address_map_stats()
{
    return get_low_address_map_debug_level() > 0;
}

static bool should_trace_low_address_map_events()
{
    return get_low_address_map_debug_level() > 1;
}

static void update_peak_stat(std::atomic<uint64_t>& peak, uint64_t value)
{
    uint64_t current = peak.load(std::memory_order_relaxed);
    while (current < value &&
           !peak.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

static std::string format_bytes(uint64_t bytes)
{
    char buffer[64];
    const double kib = 1024.0;
    const double mib = kib * 1024.0;
    const double gib = mib * 1024.0;

    if (bytes >= static_cast<uint64_t>(gib)) {
        std::snprintf(buffer, sizeof(buffer), "%.2f GiB", static_cast<double>(bytes) / gib);
    } else if (bytes >= static_cast<uint64_t>(mib)) {
        std::snprintf(buffer, sizeof(buffer), "%.2f MiB", static_cast<double>(bytes) / mib);
    } else if (bytes >= static_cast<uint64_t>(kib)) {
        std::snprintf(buffer, sizeof(buffer), "%.2f KiB", static_cast<double>(bytes) / kib);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%" PRIu64 " B", bytes);
    }

    return std::string(buffer);
}

static std::string format_duration_ms(uint64_t nanoseconds)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.3f ms", static_cast<double>(nanoseconds) / 1000000.0);
    return std::string(buffer);
}

static std::string format_pointer(const void* ptr)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%p", ptr);
    return std::string(buffer);
}

static std::string format_hex_u64(uint64_t value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%" PRIx64, value);
    return std::string(buffer);
}

static std::string format_device_memory_handle(VkDeviceMemory memory)
{
#if defined(VK_USE_64_BIT_PTR_DEFINES) && (VK_USE_64_BIT_PTR_DEFINES == 1)
    return format_pointer(memory);
#else
    return format_hex_u64(static_cast<uint64_t>(memory));
#endif
}

static const char* shadow_allocation_method_to_string(ShadowAllocationMethod method)
{
    switch (method) {
        case ShadowAllocationMethod::MAP_32BIT:
            return "MAP_32BIT";
        case ShadowAllocationMethod::FIXED_SEARCH:
            return "fixed-search";
        case ShadowAllocationMethod::NONE:
        default:
            return "none";
    }
}

static const char* low_address_copy_kind_to_string(LowAddressCopyKind kind)
{
    switch (kind) {
        case LowAddressCopyKind::MAP_TO_SHADOW:
            return "map-to-shadow";
        case LowAddressCopyKind::FLUSH_TO_REAL:
            return "flush-to-real";
        case LowAddressCopyKind::INVALIDATE_TO_SHADOW:
            return "invalidate-to-shadow";
        case LowAddressCopyKind::SUBMIT_TO_REAL:
            return "submit-to-real";
        case LowAddressCopyKind::UNMAP_TO_REAL:
            return "unmap-to-real";
        default:
            return "unknown";
    }
}

static void record_low_address_copy(LowAddressCopyKind kind, size_t size, uint64_t elapsed_ns)
{
    if (!should_collect_low_address_map_stats() || size == 0) {
        return;
    }

    low_address_map_stats.copy_time_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
    switch (kind) {
        case LowAddressCopyKind::MAP_TO_SHADOW:
            low_address_map_stats.initial_copy_bytes.fetch_add(size, std::memory_order_relaxed);
            break;
        case LowAddressCopyKind::FLUSH_TO_REAL:
            low_address_map_stats.flush_copy_bytes.fetch_add(size, std::memory_order_relaxed);
            break;
        case LowAddressCopyKind::INVALIDATE_TO_SHADOW:
            low_address_map_stats.invalidate_copy_bytes.fetch_add(size, std::memory_order_relaxed);
            break;
        case LowAddressCopyKind::SUBMIT_TO_REAL:
            low_address_map_stats.submit_copy_bytes.fetch_add(size, std::memory_order_relaxed);
            break;
        case LowAddressCopyKind::UNMAP_TO_REAL:
            low_address_map_stats.unmap_copy_bytes.fetch_add(size, std::memory_order_relaxed);
            break;
    }
}

static void tracked_memcpy(void* dst, const void* src, size_t size, LowAddressCopyKind kind)
{
    if (size == 0) {
        return;
    }

    if (!should_collect_low_address_map_stats()) {
        std::memcpy(dst, src, size);
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    std::memcpy(dst, src, size);
    const uint64_t elapsed_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
    record_low_address_copy(kind, size, elapsed_ns);

    if (should_trace_low_address_map_events()) {
        LOW_ADDRESS_LOG_DEBUG("Low-address copy: kind=" +
                              std::string(low_address_copy_kind_to_string(kind)) +
                              ", size=" + format_bytes(static_cast<uint64_t>(size)) +
                              ", src=" + format_pointer(src) +
                              ", dst=" + format_pointer(dst) +
                              ", elapsed=" + format_duration_ms(elapsed_ns));
    }
}

static void record_shadow_mapping_installed(size_t new_shadow_size,
                                            size_t replaced_shadow_size,
                                            const ShadowAllocationResult& allocation)
{
    if (!should_collect_low_address_map_stats()) {
        return;
    }

    low_address_map_stats.shadow_maps.fetch_add(1, std::memory_order_relaxed);
    low_address_map_stats.total_shadow_bytes_reserved.fetch_add(new_shadow_size, std::memory_order_relaxed);
    low_address_map_stats.allocation_time_ns.fetch_add(allocation.elapsed_ns, std::memory_order_relaxed);
    low_address_map_stats.fixed_search_attempts.fetch_add(allocation.fixed_search_attempts, std::memory_order_relaxed);

    switch (allocation.method) {
        case ShadowAllocationMethod::MAP_32BIT:
            low_address_map_stats.map32bit_allocations.fetch_add(1, std::memory_order_relaxed);
            break;
        case ShadowAllocationMethod::FIXED_SEARCH:
            low_address_map_stats.fixed_search_allocations.fetch_add(1, std::memory_order_relaxed);
            break;
        case ShadowAllocationMethod::NONE:
        default:
            break;
    }

    if (replaced_shadow_size == 0) {
        const uint64_t current_maps =
            low_address_map_stats.active_shadow_maps.fetch_add(1, std::memory_order_relaxed) + 1;
        update_peak_stat(low_address_map_stats.peak_shadow_maps, current_maps);
    }

    if (new_shadow_size > replaced_shadow_size) {
        const uint64_t delta = static_cast<uint64_t>(new_shadow_size - replaced_shadow_size);
        const uint64_t current_bytes =
            low_address_map_stats.active_shadow_bytes.fetch_add(delta, std::memory_order_relaxed) + delta;
        update_peak_stat(low_address_map_stats.peak_shadow_bytes, current_bytes);
    } else if (replaced_shadow_size > new_shadow_size) {
        low_address_map_stats.active_shadow_bytes.fetch_sub(
            static_cast<uint64_t>(replaced_shadow_size - new_shadow_size),
            std::memory_order_relaxed);
    }
}

static void record_shadow_mapping_removed(size_t shadow_size)
{
    if (!should_collect_low_address_map_stats() || shadow_size == 0) {
        return;
    }

    low_address_map_stats.active_shadow_maps.fetch_sub(1, std::memory_order_relaxed);
    low_address_map_stats.active_shadow_bytes.fetch_sub(
        static_cast<uint64_t>(shadow_size), std::memory_order_relaxed);
}

static LowAddressMapReportSnapshot capture_low_address_map_report_snapshot()
{
    LowAddressMapReportSnapshot snapshot{};
    snapshot.successful_maps =
        low_address_map_stats.successful_maps.load(std::memory_order_relaxed);
    snapshot.compatible_maps =
        low_address_map_stats.compatible_maps.load(std::memory_order_relaxed);
    snapshot.high_pointer_maps =
        low_address_map_stats.high_pointer_maps.load(std::memory_order_relaxed);
    snapshot.shadow_maps =
        low_address_map_stats.shadow_maps.load(std::memory_order_relaxed);
    snapshot.disabled_high_pointer_maps =
        low_address_map_stats.disabled_high_pointer_maps.load(std::memory_order_relaxed);
    snapshot.allocation_failures =
        low_address_map_stats.allocation_failures.load(std::memory_order_relaxed);
    snapshot.active_shadow_maps =
        low_address_map_stats.active_shadow_maps.load(std::memory_order_relaxed);
    snapshot.active_shadow_bytes =
        low_address_map_stats.active_shadow_bytes.load(std::memory_order_relaxed);

    const uint64_t initial_copy_bytes =
        low_address_map_stats.initial_copy_bytes.load(std::memory_order_relaxed);
    const uint64_t flush_copy_bytes =
        low_address_map_stats.flush_copy_bytes.load(std::memory_order_relaxed);
    const uint64_t invalidate_copy_bytes =
        low_address_map_stats.invalidate_copy_bytes.load(std::memory_order_relaxed);
    const uint64_t submit_copy_bytes =
        low_address_map_stats.submit_copy_bytes.load(std::memory_order_relaxed);
    const uint64_t unmap_copy_bytes =
        low_address_map_stats.unmap_copy_bytes.load(std::memory_order_relaxed);
    snapshot.total_copy_bytes =
        initial_copy_bytes + flush_copy_bytes + invalidate_copy_bytes + submit_copy_bytes +
        unmap_copy_bytes;

    return snapshot;
}

static bool low_address_map_report_changed(const LowAddressMapReportSnapshot& previous,
                                           const LowAddressMapReportSnapshot& current)
{
    return previous.successful_maps != current.successful_maps ||
           previous.compatible_maps != current.compatible_maps ||
           previous.high_pointer_maps != current.high_pointer_maps ||
           previous.shadow_maps != current.shadow_maps ||
           previous.disabled_high_pointer_maps != current.disabled_high_pointer_maps ||
           previous.allocation_failures != current.allocation_failures ||
           previous.active_shadow_maps != current.active_shadow_maps ||
           previous.active_shadow_bytes != current.active_shadow_bytes ||
           previous.total_copy_bytes != current.total_copy_bytes;
}

static void maybe_log_low_address_map_progress(const char* reason, bool force)
{
    if (!should_collect_low_address_map_stats()) {
        return;
    }

    const LowAddressMapReportSnapshot snapshot = capture_low_address_map_report_snapshot();
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(low_address_map_report_mutex);
        const bool has_changes =
            !low_address_map_report_initialized ||
            low_address_map_report_changed(low_address_map_last_report_snapshot, snapshot);
        if (!force) {
            if (!has_changes) {
                return;
            }

            if (low_address_map_report_initialized &&
                now - low_address_map_last_report_time < std::chrono::seconds(5)) {
                return;
            }
        }

        low_address_map_last_report_snapshot = snapshot;
        low_address_map_last_report_time = now;
        low_address_map_report_initialized = true;
    }

    std::string tag = "Low-address map progress";
    if (reason != nullptr && reason[0] != '\0') {
        tag += "[";
        tag += reason;
        tag += "]";
    }

    LOW_ADDRESS_LOG_INFO(tag +
                         ": successful_maps=" + std::to_string(snapshot.successful_maps) +
                         ", already_32bit=" + std::to_string(snapshot.compatible_maps) +
                         ", high_pointer_maps=" + std::to_string(snapshot.high_pointer_maps) +
                         ", shadow_maps=" + std::to_string(snapshot.shadow_maps) +
                         ", disabled_high_pointer_maps=" + std::to_string(snapshot.disabled_high_pointer_maps) +
                         ", allocation_failures=" + std::to_string(snapshot.allocation_failures) +
                         ", active_shadows=" + std::to_string(snapshot.active_shadow_maps) +
                         ", active_shadow_bytes=" + format_bytes(snapshot.active_shadow_bytes) +
                         ", total_copy=" + format_bytes(snapshot.total_copy_bytes));
}

static void log_low_address_map_summary()
{
    if (!should_collect_low_address_map_stats()) {
        return;
    }

    const uint64_t successful_maps =
        low_address_map_stats.successful_maps.load(std::memory_order_relaxed);
    const uint64_t compatible_maps =
        low_address_map_stats.compatible_maps.load(std::memory_order_relaxed);
    const uint64_t high_pointer_maps =
        low_address_map_stats.high_pointer_maps.load(std::memory_order_relaxed);
    const uint64_t shadow_maps =
        low_address_map_stats.shadow_maps.load(std::memory_order_relaxed);
    const uint64_t disabled_high_pointer_maps =
        low_address_map_stats.disabled_high_pointer_maps.load(std::memory_order_relaxed);
    const uint64_t resolve_failures =
        low_address_map_stats.resolve_failures.load(std::memory_order_relaxed);
    const uint64_t unsupported_size_failures =
        low_address_map_stats.unsupported_size_failures.load(std::memory_order_relaxed);
    const uint64_t allocation_failures =
        low_address_map_stats.allocation_failures.load(std::memory_order_relaxed);
    const uint64_t map32bit_allocations =
        low_address_map_stats.map32bit_allocations.load(std::memory_order_relaxed);
    const uint64_t fixed_search_allocations =
        low_address_map_stats.fixed_search_allocations.load(std::memory_order_relaxed);
    const uint64_t fixed_search_attempts =
        low_address_map_stats.fixed_search_attempts.load(std::memory_order_relaxed);
    const uint64_t total_shadow_bytes_reserved =
        low_address_map_stats.total_shadow_bytes_reserved.load(std::memory_order_relaxed);
    const uint64_t peak_shadow_maps =
        low_address_map_stats.peak_shadow_maps.load(std::memory_order_relaxed);
    const uint64_t peak_shadow_bytes =
        low_address_map_stats.peak_shadow_bytes.load(std::memory_order_relaxed);
    const uint64_t initial_copy_bytes =
        low_address_map_stats.initial_copy_bytes.load(std::memory_order_relaxed);
    const uint64_t flush_copy_bytes =
        low_address_map_stats.flush_copy_bytes.load(std::memory_order_relaxed);
    const uint64_t invalidate_copy_bytes =
        low_address_map_stats.invalidate_copy_bytes.load(std::memory_order_relaxed);
    const uint64_t submit_copy_bytes =
        low_address_map_stats.submit_copy_bytes.load(std::memory_order_relaxed);
    const uint64_t unmap_copy_bytes =
        low_address_map_stats.unmap_copy_bytes.load(std::memory_order_relaxed);
    const uint64_t allocation_time_ns =
        low_address_map_stats.allocation_time_ns.load(std::memory_order_relaxed);
    const uint64_t copy_time_ns =
        low_address_map_stats.copy_time_ns.load(std::memory_order_relaxed);
    const uint64_t total_copy_bytes =
        initial_copy_bytes + flush_copy_bytes + invalidate_copy_bytes + submit_copy_bytes +
        unmap_copy_bytes;

    LOW_ADDRESS_LOG_INFO("Low-address map summary: workaround=" +
                         std::string(should_use_low_address_shadow_map() ? "enabled" : "disabled") +
                         ", debug=" + std::to_string(get_low_address_map_debug_level()) +
                         ", successful_maps=" + std::to_string(successful_maps) +
                         ", already_32bit=" + std::to_string(compatible_maps) +
                         ", high_pointer_maps=" + std::to_string(high_pointer_maps) +
                         ", shadow_maps=" + std::to_string(shadow_maps) +
                         ", disabled_high_pointer_maps=" + std::to_string(disabled_high_pointer_maps) +
                         ", resolve_failures=" + std::to_string(resolve_failures) +
                         ", unsupported_size_failures=" + std::to_string(unsupported_size_failures) +
                         ", allocation_failures=" + std::to_string(allocation_failures));

    LOW_ADDRESS_LOG_INFO("Low-address map allocation stats: map32bit=" + std::to_string(map32bit_allocations) +
                         ", fixed_search=" + std::to_string(fixed_search_allocations) +
                         ", fixed_search_attempts=" + std::to_string(fixed_search_attempts) +
                         ", total_shadow_reserved=" + format_bytes(total_shadow_bytes_reserved) +
                         ", peak_active_shadows=" + std::to_string(peak_shadow_maps) +
                         ", peak_shadow_bytes=" + format_bytes(peak_shadow_bytes) +
                         ", allocation_time=" + format_duration_ms(allocation_time_ns));

    LOW_ADDRESS_LOG_INFO("Low-address map copy stats: initial=" + format_bytes(initial_copy_bytes) +
                         ", flush=" + format_bytes(flush_copy_bytes) +
                         ", invalidate=" + format_bytes(invalidate_copy_bytes) +
                         ", submit=" + format_bytes(submit_copy_bytes) +
                         ", unmap=" + format_bytes(unmap_copy_bytes) +
                         ", total=" + format_bytes(total_copy_bytes) +
                         ", copy_time=" + format_duration_ms(copy_time_ns));
}

struct DxvkFeatureSpoofConfig {
    bool fill_mode_non_solid = false;
    bool multi_viewport = false;
    bool shader_clip_distance = false;
    bool shader_cull_distance = false;
    bool robust_buffer_access_2 = false;
};

static const DxvkFeatureSpoofConfig& get_dxvk_feature_spoof_config()
{
    static DxvkFeatureSpoofConfig cached{};
    static bool initialized = false;
    if (initialized) {
        return cached;
    }

    bool bundle_enabled = false;
    if (getenv("MALI_WRAPPER_FAKE_DXVK_FEATURES") != nullptr) {
        bundle_enabled = is_bool_env_enabled("MALI_WRAPPER_FAKE_DXVK_FEATURES", false);
    }

    cached.fill_mode_non_solid = bundle_enabled;
    cached.multi_viewport = bundle_enabled;
    cached.shader_clip_distance = bundle_enabled;
    cached.shader_cull_distance = bundle_enabled;
    cached.robust_buffer_access_2 = bundle_enabled;

    if (getenv("MALI_WRAPPER_FAKE_FILL_MODE_NON_SOLID") != nullptr) {
        cached.fill_mode_non_solid =
            is_bool_env_enabled("MALI_WRAPPER_FAKE_FILL_MODE_NON_SOLID", cached.fill_mode_non_solid);
    }

    if (getenv("MALI_WRAPPER_FAKE_MULTI_VIEWPORT") != nullptr) {
        cached.multi_viewport =
            is_bool_env_enabled("MALI_WRAPPER_FAKE_MULTI_VIEWPORT", cached.multi_viewport);
    }

    if (getenv("MALI_WRAPPER_FAKE_SHADER_CLIP_DISTANCE") != nullptr) {
        cached.shader_clip_distance =
            is_bool_env_enabled("MALI_WRAPPER_FAKE_SHADER_CLIP_DISTANCE", cached.shader_clip_distance);
    }

    if (getenv("MALI_WRAPPER_FAKE_SHADER_CULL_DISTANCE") != nullptr) {
        cached.shader_cull_distance =
            is_bool_env_enabled("MALI_WRAPPER_FAKE_SHADER_CULL_DISTANCE", cached.shader_cull_distance);
    }

    if (getenv("MALI_WRAPPER_FAKE_ROBUST_BUFFER_ACCESS_2") != nullptr) {
        cached.robust_buffer_access_2 =
            is_bool_env_enabled("MALI_WRAPPER_FAKE_ROBUST_BUFFER_ACCESS_2", cached.robust_buffer_access_2);
    }

    if (cached.fill_mode_non_solid || cached.multi_viewport ||
        cached.shader_clip_distance || cached.shader_cull_distance ||
        cached.robust_buffer_access_2) {
        std::string features;
        if (cached.fill_mode_non_solid) {
            features += "fillModeNonSolid";
        }
        if (cached.multi_viewport) {
            if (!features.empty()) {
                features += ", ";
            }
            features += "multiViewport";
        }
        if (cached.shader_clip_distance) {
            if (!features.empty()) {
                features += ", ";
            }
            features += "shaderClipDistance";
        }
        if (cached.shader_cull_distance) {
            if (!features.empty()) {
                features += ", ";
            }
            features += "shaderCullDistance";
        }
        if (cached.robust_buffer_access_2) {
            if (!features.empty()) {
                features += ", ";
            }
            features += "robustBufferAccess2";
        }

        LOG_WARN("DXVK feature spoof enabled: advertising " + features);
    }

    initialized = true;
    return cached;
}

static bool is_any_dxvk_feature_spoof_enabled()
{
    const auto& config = get_dxvk_feature_spoof_config();
    return config.fill_mode_non_solid || config.multi_viewport ||
           config.shader_clip_distance || config.shader_cull_distance ||
           config.robust_buffer_access_2;
}

static void advertise_spoofed_physical_features(VkPhysicalDeviceFeatures* features)
{
    if (features == nullptr) {
        return;
    }

    const auto& config = get_dxvk_feature_spoof_config();
    if (config.fill_mode_non_solid) {
        features->fillModeNonSolid = VK_TRUE;
    }
    if (config.multi_viewport) {
        features->multiViewport = VK_TRUE;
    }
    if (config.shader_clip_distance) {
        features->shaderClipDistance = VK_TRUE;
    }
    if (config.shader_cull_distance) {
        features->shaderCullDistance = VK_TRUE;
    }
}

static void advertise_spoofed_physical_feature_chain(void* pnext)
{
    if (pnext == nullptr) {
        return;
    }

    const auto& config = get_dxvk_feature_spoof_config();
    auto* current = reinterpret_cast<VkBaseOutStructure*>(pnext);
    while (current != nullptr) {
        if (config.robust_buffer_access_2 &&
            current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
            auto* robustness2 = reinterpret_cast<VkPhysicalDeviceRobustness2FeaturesEXT*>(current);
            robustness2->robustBufferAccess2 = VK_TRUE;
        }

        current = current->pNext;
    }
}

static void sanitize_requested_physical_features_for_driver(VkPhysicalDeviceFeatures* features)
{
    if (features == nullptr) {
        return;
    }

    const auto& config = get_dxvk_feature_spoof_config();
    if (config.fill_mode_non_solid) {
        features->fillModeNonSolid = VK_FALSE;
    }
    if (config.multi_viewport) {
        features->multiViewport = VK_FALSE;
    }
    if (config.shader_clip_distance) {
        features->shaderClipDistance = VK_FALSE;
    }
    if (config.shader_cull_distance) {
        features->shaderCullDistance = VK_FALSE;
    }
}

static void sanitize_requested_physical_feature_chain_for_driver(const void* pnext)
{
    if (pnext == nullptr) {
        return;
    }

    const auto& config = get_dxvk_feature_spoof_config();
    auto* current = const_cast<VkBaseInStructure*>(reinterpret_cast<const VkBaseInStructure*>(pnext));
    while (current != nullptr) {
        if (config.robust_buffer_access_2 &&
            current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
            auto* robustness2 = reinterpret_cast<VkPhysicalDeviceRobustness2FeaturesEXT*>(current);
            robustness2->robustBufferAccess2 = VK_FALSE;
        }

        current = const_cast<VkBaseInStructure*>(current->pNext);
    }
}

static bool is_pointer_32bit_compatible(const void* ptr)
{
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr)) < kMax32BitAddressExclusive;
}

static bool resolve_map_size_locked(const DeviceMemoryKey& key, VkDeviceSize offset,
                                    VkDeviceSize requested_size, VkDeviceSize* out_size)
{
    if (out_size == nullptr) {
        return false;
    }

    if (requested_size != VK_WHOLE_SIZE) {
        *out_size = requested_size;
        return requested_size > 0;
    }

    auto alloc_it = tracked_memory_allocations.find(key);
    if (alloc_it == tracked_memory_allocations.end()) {
        return false;
    }

    const VkDeviceSize allocation_size = alloc_it->second;
    if (offset >= allocation_size) {
        return false;
    }

    *out_size = allocation_size - offset;
    return *out_size > 0;
}

static bool compute_copy_region(const ShadowMappingInfo& mapping, VkDeviceSize range_offset,
                                VkDeviceSize range_size, size_t* out_offset, size_t* out_size)
{
    if (mapping.shadow_ptr == nullptr || mapping.real_ptr == nullptr || mapping.mapped_size == 0) {
        return false;
    }

    if (range_offset < mapping.offset) {
        return false;
    }

    const VkDeviceSize local_offset = range_offset - mapping.offset;
    if (local_offset >= mapping.mapped_size) {
        return false;
    }

    VkDeviceSize copy_size = (range_size == VK_WHOLE_SIZE) ? (mapping.mapped_size - local_offset) : range_size;
    const VkDeviceSize max_size = mapping.mapped_size - local_offset;
    if (copy_size > max_size) {
        copy_size = max_size;
    }

    if (copy_size == 0 ||
        local_offset > static_cast<VkDeviceSize>(std::numeric_limits<size_t>::max()) ||
        copy_size > static_cast<VkDeviceSize>(std::numeric_limits<size_t>::max())) {
        return false;
    }

    *out_offset = static_cast<size_t>(local_offset);
    *out_size = static_cast<size_t>(copy_size);
    return true;
}

static ShadowAllocationResult allocate_low_address_shadow(size_t requested_size)
{
    ShadowAllocationResult result{};
    if (requested_size == 0) {
        return result;
    }

    long page_size_value = sysconf(_SC_PAGESIZE);
    if (page_size_value <= 0) {
        page_size_value = 4096;
    }

    const size_t page_size = static_cast<size_t>(page_size_value);
    const size_t aligned_size = ((requested_size + page_size - 1) / page_size) * page_size;
    if (aligned_size == 0 || aligned_size < requested_size) {
        return result;
    }

    if (should_trace_low_address_map_events()) {
        LOW_ADDRESS_LOG_DEBUG("Low-address allocation request: requested=" +
                              format_bytes(static_cast<uint64_t>(requested_size)) +
                              ", aligned=" + format_bytes(static_cast<uint64_t>(aligned_size)));
    }

    const bool collect_stats = should_collect_low_address_map_stats();
    const auto start_time = collect_stats ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};

#ifdef MAP_32BIT
    void* mapped = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (mapped != MAP_FAILED) {
        const uint64_t mapped_addr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mapped));
        const uint64_t mapped_end = mapped_addr + static_cast<uint64_t>(aligned_size);
        if (mapped_end <= kMax32BitAddressExclusive) {
            result.ptr = mapped;
            result.size = aligned_size;
            result.method = ShadowAllocationMethod::MAP_32BIT;
            if (collect_stats) {
                result.elapsed_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - start_time).count());
            }
            return result;
        }
        if (should_trace_low_address_map_events()) {
            LOW_ADDRESS_LOG_DEBUG("Low-address MAP_32BIT candidate discarded: ptr=" +
                                  format_pointer(mapped) +
                                  ", size=" + format_bytes(static_cast<uint64_t>(aligned_size)));
        }
        munmap(mapped, aligned_size);
    } else {
        result.last_errno = errno;
    }
#endif

    for (uintptr_t addr = kShadowSearchStart;
         addr < kShadowSearchEnd &&
             static_cast<uint64_t>(addr) + static_cast<uint64_t>(aligned_size) < kMax32BitAddressExclusive;
         addr += kShadowSearchStep) {
        result.fixed_search_attempts++;
        void* mapped = mmap(reinterpret_cast<void*>(addr), aligned_size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (mapped != MAP_FAILED) {
            result.ptr = mapped;
            result.size = aligned_size;
            result.method = ShadowAllocationMethod::FIXED_SEARCH;
            if (collect_stats) {
                result.elapsed_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - start_time).count());
            }
            return result;
        }

        result.last_errno = errno;
        if (result.last_errno != EEXIST && result.last_errno != EINVAL &&
            result.last_errno != ENOMEM && result.last_errno != EBUSY) {
            break;
        }
    }

    if (collect_stats) {
        result.elapsed_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start_time).count());
    }
    return result;
}

static void remove_tracking_for_device(VkDevice device)
{
    std::vector<ShadowMappingInfo> stale_mappings;

    {
        std::lock_guard<std::mutex> lock(memory_tracking_mutex);
        for (auto it = shadow_mappings.begin(); it != shadow_mappings.end(); ) {
            if (it->first.device == device) {
                stale_mappings.push_back(it->second);
                it = shadow_mappings.erase(it);
            } else {
                ++it;
            }
        }

        for (auto it = tracked_memory_allocations.begin(); it != tracked_memory_allocations.end(); ) {
            if (it->first.device == device) {
                it = tracked_memory_allocations.erase(it);
            } else {
                ++it;
            }
        }
    }

    uint64_t released_shadow_bytes = 0;
    for (const auto& mapping : stale_mappings) {
        if (mapping.shadow_ptr != nullptr && mapping.shadow_size > 0) {
            released_shadow_bytes += static_cast<uint64_t>(mapping.shadow_size);
            record_shadow_mapping_removed(mapping.shadow_size);
            munmap(mapping.shadow_ptr, mapping.shadow_size);
        }
    }

    if (!stale_mappings.empty()) {
        LOW_ADDRESS_LOG_INFO("Low-address device cleanup: device=" + format_pointer(device) +
                             ", stale_mappings=" + std::to_string(stale_mappings.size()) +
                             ", released=" + format_bytes(released_shadow_bytes));
    }
}

void add_instance_reference(VkInstance instance) {
    std::lock_guard<std::mutex> lock(instance_mutex);
    auto it = managed_instances.find(instance);
    if (it != managed_instances.end()) {
        it->second->ref_count++;
    }
}

void remove_instance_reference(VkInstance instance) {
    bool should_cleanup = false;
    {
        std::lock_guard<std::mutex> lock(instance_mutex);
        auto it = managed_instances.find(instance);
        if (it != managed_instances.end()) {
            it->second->ref_count--;

            if (it->second->marked_for_destruction && it->second->ref_count <= 0) {
                should_cleanup = true;
                managed_instances.erase(it);
            }
        }
    }

    if (should_cleanup) {
        LOG_INFO("Performing delayed instance cleanup for instance with 0 references");
        GetWSIManager().release_instance(instance);
    }
}

bool is_instance_valid(VkInstance instance) {
    std::lock_guard<std::mutex> lock(instance_mutex);
    auto it = managed_instances.find(instance);
    return (it != managed_instances.end() && !it->second->marked_for_destruction);
}

static VkInstance get_device_parent_instance(VkDevice device)
{
    auto it = managed_devices.find(device);
    if (it != managed_devices.end())
    {
        return it->second;
    }

    std::lock_guard<std::mutex> lock(instance_mutex);
    if (latest_instance != VK_NULL_HANDLE)
    {
        auto latest_it = managed_instances.find(latest_instance);
        if (latest_it != managed_instances.end())
        {
            return latest_it->second->instance;
        }
    }

    if (!managed_instances.empty())
    {
        return managed_instances.begin()->second->instance;
    }

    return VK_NULL_HANDLE;
}

static VkDevice get_any_managed_device()
{
    std::lock_guard<std::mutex> lock(instance_mutex);
    if (managed_devices.empty()) {
        return VK_NULL_HANDLE;
    }

    return managed_devices.begin()->first;
}

static VkInstance get_any_managed_instance()
{
    std::lock_guard<std::mutex> lock(instance_mutex);
    if (latest_instance != VK_NULL_HANDLE) {
        auto latest_it = managed_instances.find(latest_instance);
        if (latest_it != managed_instances.end()) {
            return latest_it->second->instance;
        }
    }

    if (managed_instances.empty()) {
        return VK_NULL_HANDLE;
    }

    return managed_instances.begin()->second->instance;
}

template <typename T>
static T get_mali_instance_proc_for_physical_device(VkPhysicalDevice physicalDevice, const char* proc_name)
{
    auto mali_proc_addr = LibraryLoader::Instance().GetMaliGetInstanceProcAddr();
    if (mali_proc_addr == nullptr || proc_name == nullptr) {
        return nullptr;
    }

    VkInstance instance = VK_NULL_HANDLE;
    try {
        auto& instance_data = instance_private_data::get(physicalDevice);
        instance = instance_data.get_instance_handle();
    } catch (...) {
        instance = VK_NULL_HANDLE;
    }

    if (instance == VK_NULL_HANDLE) {
        instance = get_any_managed_instance();
    }

    if (instance != VK_NULL_HANDLE) {
        auto proc = mali_proc_addr(instance, proc_name);
        if (proc != nullptr) {
            return reinterpret_cast<T>(proc);
        }
    }

    auto global_proc = mali_proc_addr(VK_NULL_HANDLE, proc_name);
    if (global_proc != nullptr) {
        return reinterpret_cast<T>(global_proc);
    }

    return reinterpret_cast<T>(LibraryLoader::Instance().GetMaliProcAddr(proc_name));
}

struct DeviceCreateInfoFeatureSpoofCopies {
    VkPhysicalDeviceFeatures enabled_features{};
    VkPhysicalDeviceFeatures2 features2{};
};

static void sanitize_device_create_info_for_driver(VkDeviceCreateInfo* create_info,
                                                   DeviceCreateInfoFeatureSpoofCopies* spoof_copies)
{
    if (create_info == nullptr || spoof_copies == nullptr || !is_any_dxvk_feature_spoof_enabled()) {
        return;
    }

    if (create_info->pEnabledFeatures != nullptr) {
        spoof_copies->enabled_features = *create_info->pEnabledFeatures;
        sanitize_requested_physical_features_for_driver(&spoof_copies->enabled_features);
        create_info->pEnabledFeatures = &spoof_copies->enabled_features;
    }

    const auto* base = reinterpret_cast<const VkBaseInStructure*>(create_info->pNext);
    if (base != nullptr && base->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
        spoof_copies->features2 = *reinterpret_cast<const VkPhysicalDeviceFeatures2*>(base);
        sanitize_requested_physical_features_for_driver(&spoof_copies->features2.features);
        sanitize_requested_physical_feature_chain_for_driver(spoof_copies->features2.pNext);
        create_info->pNext = &spoof_copies->features2;
    } else {
        sanitize_requested_physical_feature_chain_for_driver(create_info->pNext);
    }
}


static const std::unordered_set<std::string> wsi_functions = {
    // Surface functions
    "vkCreateXlibSurfaceKHR",
    "vkCreateXcbSurfaceKHR",
    "vkCreateWaylandSurfaceKHR",
    "vkCreateDisplaySurfaceKHR",
    "vkCreateHeadlessSurfaceEXT",
    "vkDestroySurfaceKHR",
    "vkGetPhysicalDeviceSurfaceSupportKHR",
    "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
    "vkGetPhysicalDeviceSurfaceCapabilities2KHR",
    "vkGetPhysicalDeviceSurfaceFormatsKHR",
    "vkGetPhysicalDeviceSurfaceFormats2KHR",
    "vkGetPhysicalDeviceSurfacePresentModesKHR",

    // Swapchain functions
    "vkCreateSwapchainKHR",
    "vkCreateSharedSwapchainsKHR",
    "vkDestroySwapchainKHR",
    "vkGetSwapchainImagesKHR",
    "vkAcquireNextImageKHR",
    "vkAcquireNextImage2KHR",
    "vkQueuePresentKHR",
    "vkGetSwapchainStatusKHR",
    "vkReleaseSwapchainImagesEXT",

    // Display functions
    "vkGetPhysicalDeviceDisplayPropertiesKHR",
    "vkGetPhysicalDeviceDisplayProperties2KHR",
    "vkGetPhysicalDeviceDisplayPlanePropertiesKHR",
    "vkGetPhysicalDeviceDisplayPlaneProperties2KHR",
    "vkGetDisplayPlaneSupportedDisplaysKHR",
    "vkGetDisplayModePropertiesKHR",
    "vkGetDisplayModeProperties2KHR",
    "vkCreateDisplayModeKHR",
    "vkGetDisplayPlaneCapabilitiesKHR",
    "vkGetDisplayPlaneCapabilities2KHR",

    // Present timing functions
    "vkGetSwapchainTimingPropertiesEXT",
    "vkGetSwapchainTimeDomainPropertiesEXT",
    "vkGetPastPresentationTimingEXT",
    "vkSetSwapchainPresentTimingQueueSizeEXT",

    // Presentation support functions
    "vkGetPhysicalDeviceWaylandPresentationSupportKHR",
    "vkGetPhysicalDeviceXlibPresentationSupportKHR",
    "vkGetPhysicalDeviceXcbPresentationSupportKHR"
};

static bool IsWSIFunction(const char* name) {
    return wsi_functions.find(name) != wsi_functions.end();
}

static bool should_install_crash_handler()
{
    // Keep crash handler opt-in because it can interfere with app/runtime handlers.
    return is_bool_env_enabled("MALI_WRAPPER_CRASH_SIGNAL_HANDLER", false);
}

static bool should_filter_external_memory_host_extension()
{
    static int cached = -1;
    if (cached >= 0) {
        return cached == 1;
    }

    const bool forced = getenv("MALI_WRAPPER_FILTER_EXTERNAL_MEMORY_HOST") != nullptr;
    if (forced) {
        cached = is_bool_env_enabled("MALI_WRAPPER_FILTER_EXTERNAL_MEMORY_HOST", false) ? 1 : 0;
    } else {
        cached = (getenv("WINEWOW64") != nullptr || getenv("WINE_WOW64") != nullptr) ? 1 : 0;
    }

    if (cached == 1) {
        LOG_WARN("Filtering VK_EXT_external_memory_host from device extension list");
    }
    return cached == 1;
}

static bool is_filtered_device_extension(const char* extension_name)
{
    if (extension_name == nullptr) {
        return false;
    }

    return should_filter_external_memory_host_extension() &&
           strcmp(extension_name, "VK_EXT_external_memory_host") == 0;
}

static size_t filter_device_extension_vector(std::vector<const char*>* extensions)
{
    if (extensions == nullptr || extensions->empty()) {
        return 0;
    }

    const size_t before = extensions->size();
    extensions->erase(
        std::remove_if(extensions->begin(), extensions->end(),
                       [](const char* extension_name) {
                           return is_filtered_device_extension(extension_name);
                       }),
        extensions->end());
    return before - extensions->size();
}

static bool should_guard_graphics_pipeline_create_with_signals()
{
    static int cached = -1;
    if (cached >= 0) {
        return cached == 1;
    }

    // Keep this opt-in: replacing process-wide SIGSEGV/SIGBUS handlers can
    // break runtimes that rely on signal handlers (e.g. Wine/Box64).
    bool default_enabled = false;
    cached = is_bool_env_enabled("MALI_WRAPPER_GRAPHICS_PIPELINE_SIGNAL_GUARD", default_enabled) ? 1 : 0;
    if (cached == 1) {
        LOG_WARN("vkCreateGraphicsPipelines signal guard is enabled");
    }
    return cached == 1;
}

static void graphics_pipeline_signal_guard_handler(int sig, siginfo_t* /*info*/, void* /*ctx*/)
{
    if (graphics_pipeline_signal_guard_active) {
        graphics_pipeline_signal_guard_caught_signal = sig;
        siglongjmp(graphics_pipeline_signal_guard_env, 1);
    }

    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
    raise(sig);
}

static VkResult call_vkCreateGraphicsPipelines_with_signal_guard(
    PFN_vkCreateGraphicsPipelines mali_create_graphics_pipelines,
    VkDevice device,
    VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines,
    bool* trapped_signal)
{
    if (trapped_signal != nullptr) {
        *trapped_signal = false;
    }

    if (!should_guard_graphics_pipeline_create_with_signals()) {
        return mali_create_graphics_pipelines(
            device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
    }

    std::lock_guard<std::mutex> lock(graphics_pipeline_signal_guard_mutex);

    struct sigaction guard_action{};
    guard_action.sa_sigaction = graphics_pipeline_signal_guard_handler;
    sigemptyset(&guard_action.sa_mask);
    guard_action.sa_flags = SA_SIGINFO;

    struct sigaction old_segv{};
    struct sigaction old_bus{};
    sigaction(SIGSEGV, &guard_action, &old_segv);
    sigaction(SIGBUS, &guard_action, &old_bus);

    graphics_pipeline_signal_guard_caught_signal = 0;
    graphics_pipeline_signal_guard_active = 1;

    VkResult result = VK_ERROR_FEATURE_NOT_PRESENT;
    if (sigsetjmp(graphics_pipeline_signal_guard_env, 1) == 0) {
        result = mali_create_graphics_pipelines(
            device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
    } else if (trapped_signal != nullptr) {
        *trapped_signal = true;
    }

    graphics_pipeline_signal_guard_active = 0;
    sigaction(SIGSEGV, &old_segv, nullptr);
    sigaction(SIGBUS, &old_bus, nullptr);

    if (trapped_signal != nullptr && *trapped_signal) {
        LOG_ERROR("vkCreateGraphicsPipelines trapped signal " +
                  std::to_string(static_cast<int>(graphics_pipeline_signal_guard_caught_signal)) +
                  "; returning VK_ERROR_FEATURE_NOT_PRESENT");
    }

    return result;
}

static void crash_signal_handler(int sig, siginfo_t* info, void* /*ctx*/)
{
    (void)info;
    void* frames[64];
    int n = backtrace(frames, 64);
    // Use write() - async-signal-safe unlike fprintf
    const char* header = "[mali-wrapper] CRASH SIGNAL ";
    write(STDERR_FILENO, header, __builtin_strlen(header));
    char sigbuf[4];
    int len = 0;
    int s = sig;
    if (s == 0) { sigbuf[len++] = '0'; }
    else { while (s > 0) { sigbuf[len++] = '0' + (s % 10); s /= 10; } }
    // reverse
    for (int i = 0, j = len-1; i < j; i++, j--) { char t = sigbuf[i]; sigbuf[i] = sigbuf[j]; sigbuf[j] = t; }
    write(STDERR_FILENO, sigbuf, len);
    write(STDERR_FILENO, "\n", 1);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    // Re-raise with default handler to produce core dump
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
    raise(sig);
}

bool InitializeWrapper() {
    if (getenv("MALI_WRAPPER_DEBUG")) {
        Logger::Instance().SetLevel(LogLevel::DEBUG);
    }

    if (should_collect_low_address_map_stats()) {
        LOW_ADDRESS_LOG_INFO("Low-address map debug enabled: level=" +
                             std::to_string(get_low_address_map_debug_level()) +
                             ", workaround=" +
                             std::string(should_use_low_address_shadow_map() ? "enabled" : "disabled") +
                             ". A summary will be emitted when the wrapper unloads.");
    }

    if (should_install_crash_handler()) {
        struct sigaction sa{};
        sa.sa_sigaction = crash_signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
        sigaction(SIGSEGV, &sa, nullptr);
        sigaction(SIGABRT, &sa, nullptr);
        sigaction(SIGBUS,  &sa, nullptr);
        sigaction(SIGILL,  &sa, nullptr);
        LOG_INFO("Crash signal handler enabled (MALI_WRAPPER_CRASH_SIGNAL_HANDLER)");
    }

    LOG_INFO("Initializing Mali Wrapper ICD");

    if (!LibraryLoader::Instance().LoadLibraries()) {
        LOG_ERROR("Failed to load required libraries - continuing with reduced functionality");
        LOG_WARN("Extension enumeration and WSI functionality may be limited");
    }

    LOG_INFO("Mali Wrapper ICD initialized successfully");
    return true;
}

void ShutdownWrapper() {
    log_low_address_map_summary();
    LOG_INFO("Shutting down Mali Wrapper ICD");
    GetWSIManager().cleanup();
    LibraryLoader::Instance().UnloadLibraries();
}


} // namespace mali_wrapper

static VKAPI_ATTR VkResult VKAPI_CALL dummy_set_instance_loader_data(VkInstance instance, void* object) {
    return VK_SUCCESS;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL internal_vkGetDeviceProcAddr(VkDevice device, const char* pName);
static VKAPI_ATTR VkResult VKAPI_CALL internal_vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice);
static VKAPI_ATTR void VKAPI_CALL internal_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator);
static VKAPI_ATTR VkResult VKAPI_CALL internal_vkCreateImage(VkDevice device, const VkImageCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImage* pImage);
static VKAPI_ATTR VkResult VKAPI_CALL internal_vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines);
static VKAPI_ATTR VkResult VKAPI_CALL internal_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties);
static VKAPI_ATTR void VKAPI_CALL internal_vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures* pFeatures);
static VKAPI_ATTR void VKAPI_CALL internal_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures);
static VKAPI_ATTR void VKAPI_CALL internal_vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2KHR* pFeatures);
static VKAPI_ATTR VkResult VKAPI_CALL mali_driver_create_device(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice);
static VKAPI_ATTR VkResult VKAPI_CALL internal_vkAllocateMemory(
    VkDevice device,
    const VkMemoryAllocateInfo* pAllocateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDeviceMemory* pMemory);
static VKAPI_ATTR void VKAPI_CALL internal_vkFreeMemory(
    VkDevice device,
    VkDeviceMemory memory,
    const VkAllocationCallbacks* pAllocator);
static VKAPI_ATTR VkResult VKAPI_CALL internal_vkMapMemory(
    VkDevice device,
    VkDeviceMemory memory,
    VkDeviceSize offset,
    VkDeviceSize size,
    VkMemoryMapFlags flags,
    void** ppData);
static VKAPI_ATTR void VKAPI_CALL internal_vkUnmapMemory(
    VkDevice device,
    VkDeviceMemory memory);
static VKAPI_ATTR VkResult VKAPI_CALL internal_vkFlushMappedMemoryRanges(
    VkDevice device,
    uint32_t memoryRangeCount,
    const VkMappedMemoryRange* pMemoryRanges);
static VKAPI_ATTR VkResult VKAPI_CALL internal_vkInvalidateMappedMemoryRanges(
    VkDevice device,
    uint32_t memoryRangeCount,
    const VkMappedMemoryRange* pMemoryRanges);
static VKAPI_ATTR VkResult VKAPI_CALL internal_vkMapMemory2KHR(
    VkDevice device,
    const VkMemoryMapInfoKHR* pMemoryMapInfo,
    void** ppData);
static VKAPI_ATTR VkResult VKAPI_CALL internal_vkUnmapMemory2KHR(
    VkDevice device,
    const VkMemoryUnmapInfoKHR* pMemoryUnmapInfo);
static VKAPI_ATTR VkResult VKAPI_CALL internal_vkQueueSubmit(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence fence);
static VKAPI_ATTR VkResult VKAPI_CALL internal_vkQueueSubmit2(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo2* pSubmits,
    VkFence fence);
static VKAPI_ATTR VkResult VKAPI_CALL internal_vkQueueSubmit2KHR(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo2KHR* pSubmits,
    VkFence fence);

static VKAPI_ATTR VkResult VKAPI_CALL wrapper_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pSwapchainCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL filtered_mali_get_instance_proc_addr(VkInstance instance, const char* pName) {
    using namespace mali_wrapper;

    if (!pName) {
        return nullptr;
    }


    if (IsWSIFunction(pName)) {
        return nullptr;
    }

    if (strcmp(pName, "vkCreateDevice") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(mali_driver_create_device);
    }

    auto mali_proc_addr = LibraryLoader::Instance().GetMaliGetInstanceProcAddr();
    if (mali_proc_addr) {
        auto func = mali_proc_addr(instance, pName);
        return func;
    }

    return nullptr;
}

static VKAPI_ATTR VkResult VKAPI_CALL internal_vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {

    using namespace mali_wrapper;


    if (!pCreateInfo || !pInstance) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    std::vector<const char *> enabled_extensions;
    std::unique_ptr<util::extension_list> instance_extension_list;
    util::wsi_platform_set enabled_platforms;

#if BUILD_WSI_X11
    enabled_platforms.add(VK_ICD_WSI_PLATFORM_XCB);
    enabled_platforms.add(VK_ICD_WSI_PLATFORM_XLIB);
#endif
#if BUILD_WSI_WAYLAND
    enabled_platforms.add(VK_ICD_WSI_PLATFORM_WAYLAND);
#endif
#if BUILD_WSI_HEADLESS
    enabled_platforms.add(VK_ICD_WSI_PLATFORM_HEADLESS);
#endif

    try
    {
        util::allocator base_allocator = util::allocator::get_generic();
        util::allocator extension_allocator(base_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
        instance_extension_list = std::make_unique<util::extension_list>(extension_allocator);
        auto &extensions = *instance_extension_list;

        if (pCreateInfo->enabledExtensionCount > 0 && pCreateInfo->ppEnabledExtensionNames != nullptr)
        {
            extensions.add(pCreateInfo->ppEnabledExtensionNames, pCreateInfo->enabledExtensionCount);
        }

        VkResult extension_result = wsi::add_instance_extensions_required_by_layer(enabled_platforms, extensions);
        if (extension_result != VK_SUCCESS)
        {
            LOG_ERROR("Failed to collect WSI-required instance extensions, error: " +
                      std::to_string(extension_result));
            return extension_result;
        }

        util::vector<const char *> extension_vector(extension_allocator);
        extensions.get_extension_strings(extension_vector);

        std::unordered_set<std::string> seen_extensions;
        seen_extensions.reserve(extension_vector.size());

        for (const char *name : extension_vector)
        {
            if (name == nullptr)
            {
                continue;
            }
            auto inserted = seen_extensions.emplace(name);
            if (inserted.second)
            {
                enabled_extensions.push_back(name);
            }
        }
    }
    catch (const std::exception &e)
    {
        LOG_WARN(std::string("Unable to augment instance extensions: ") + e.what());
        enabled_extensions.clear();
        instance_extension_list.reset();
    }

    const char *const *instance_extension_ptr = pCreateInfo->ppEnabledExtensionNames;
    size_t instance_extension_count = pCreateInfo->enabledExtensionCount;

    if (!enabled_extensions.empty())
    {
        instance_extension_ptr = enabled_extensions.data();
        instance_extension_count = enabled_extensions.size();
    }

    VkInstanceCreateInfo modified_create_info = *pCreateInfo;
    modified_create_info.enabledExtensionCount = static_cast<uint32_t>(instance_extension_count);
    modified_create_info.ppEnabledExtensionNames = instance_extension_ptr;

    auto mali_create_instance = LibraryLoader::Instance().GetMaliCreateInstance();
    if (!mali_create_instance) {
        LOG_ERROR("Mali driver not available for instance creation");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult result = mali_create_instance(&modified_create_info, pAllocator, pInstance);

    if (result == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(instance_mutex);
        auto existing = managed_instances.find(*pInstance);
        if (existing == managed_instances.end()) {
            managed_instances.emplace(*pInstance, std::make_unique<InstanceInfo>(*pInstance));
        } else {
            LOG_WARN("Instance handle reused - resetting tracking state");
            existing->second->instance = *pInstance;
            existing->second->ref_count = 1;
            existing->second->marked_for_destruction = false;
            existing->second->destroy_time = {};
        }
        latest_instance = *pInstance;

        VkResult wsi_result = GetWSIManager().initialize(*pInstance, VK_NULL_HANDLE);
        if (wsi_result != VK_SUCCESS) {
            LOG_ERROR("Failed to initialize WSI manager for instance, error: " + std::to_string(wsi_result));
        }

        try
        {
            auto &instance_data = instance_private_data::get(*pInstance);
            if (instance_extension_ptr != nullptr && instance_extension_count > 0)
            {
                instance_data.set_instance_enabled_extensions(instance_extension_ptr, instance_extension_count);
            }
        }
        catch (const std::exception &e)
        {
            LOG_WARN(std::string("Failed to record enabled instance extensions: ") + e.what());
        }

        LOG_INFO("Instance created successfully through WSI layer -> Mali driver chain");
    } else {
        LOG_ERROR("Failed to create instance through WSI layer, error: " + std::to_string(result));
    }

    return result;
}

static VKAPI_ATTR void VKAPI_CALL internal_vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator) {

    using namespace mali_wrapper;


    if (instance == VK_NULL_HANDLE) {
        return;
    }

    std::unique_ptr<InstanceInfo> instance_info;
    {
        std::lock_guard<std::mutex> lock(instance_mutex);
        auto it = managed_instances.find(instance);
        if (it == managed_instances.end()) {
            LOG_WARN("Destroying unmanaged instance");
            return;
        }

        it->second->marked_for_destruction = true;
        it->second->destroy_time = std::chrono::steady_clock::now();

        LOG_INFO("Instance marked for destruction with ref_count=" + std::to_string(it->second->ref_count));

        if (it->second->ref_count > 0) {
            LOG_WARN("Instance has " + std::to_string(it->second->ref_count) +
                     " active references - deferring cleanup to prevent race conditions");
            return; // Don't destroy yet, let reference cleanup handle it
        }

        instance_info = std::move(it->second);
        managed_instances.erase(it);
        if (latest_instance == instance) {
            latest_instance = VK_NULL_HANDLE;
            if (!managed_instances.empty()) {
                latest_instance = managed_instances.begin()->second->instance;
            }
        }
    }

    // Cleanup associated devices
    for (auto dev_it = managed_devices.begin(); dev_it != managed_devices.end(); ) {
        if (dev_it->second == instance) {
            GetWSIManager().release_device(dev_it->first);
            dev_it = managed_devices.erase(dev_it);
        } else {
            ++dev_it;
        }
    }

    auto mali_proc_addr = LibraryLoader::Instance().GetMaliGetInstanceProcAddr();
    if (mali_proc_addr) {
        auto mali_destroy = reinterpret_cast<PFN_vkDestroyInstance>(
            mali_proc_addr(instance, "vkDestroyInstance"));
        if (mali_destroy) {
            mali_destroy(instance, pAllocator);
        }
    }

    GetWSIManager().release_instance(instance);
    LOG_INFO("Instance destroyed successfully");
}

static VKAPI_ATTR VkResult VKAPI_CALL internal_vkEnumerateInstanceExtensionProperties(
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {

    using namespace mali_wrapper;


    if (pLayerName != nullptr) {
        *pPropertyCount = 0;
        return VK_SUCCESS;
    }

    std::vector<VkExtensionProperties> mali_extensions;

    if (LibraryLoader::Instance().IsLoaded()) {
        auto mali_enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            LibraryLoader::Instance().GetMaliProcAddr("vkEnumerateInstanceExtensionProperties"));
        if (mali_enumerate) {
            uint32_t mali_count = 0;
            VkResult result = mali_enumerate(nullptr, &mali_count, nullptr);
            if (result == VK_SUCCESS && mali_count > 0) {
                mali_extensions.resize(mali_count);
                mali_enumerate(nullptr, &mali_count, mali_extensions.data());
            }
        }
    }

    std::vector<VkExtensionProperties> wsi_extensions;
    bool wsi_available = false;
    try {
        wsi_available = LibraryLoader::Instance().IsLoaded();
    } catch (...) {
        LOG_WARN("Exception checking WSI availability during extension enumeration");
        wsi_available = false;
    }

    if (wsi_available) {
        const char* wsi_extension_names[] = {
            "VK_KHR_surface",
            "VK_KHR_wayland_surface",
            "VK_KHR_xcb_surface",
            "VK_KHR_xlib_surface",
            "VK_KHR_get_surface_capabilities2",
            "VK_EXT_surface_maintenance1",
            "VK_EXT_headless_surface"
        };

        for (const char* ext_name : wsi_extension_names) {
            VkExtensionProperties ext = {};
            strncpy(ext.extensionName, ext_name, VK_MAX_EXTENSION_NAME_SIZE - 1);
            ext.specVersion = 1;  // Default spec version
            wsi_extensions.push_back(ext);
        }

    }

    std::vector<VkExtensionProperties> combined_extensions = mali_extensions;
    for (const auto& wsi_ext : wsi_extensions) {
        bool found = false;
        for (const auto& mali_ext : mali_extensions) {
            if (strcmp(wsi_ext.extensionName, mali_ext.extensionName) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            combined_extensions.push_back(wsi_ext);
        }
    }


    if (pProperties == nullptr) {
        *pPropertyCount = combined_extensions.size();
        return VK_SUCCESS;
    }

    uint32_t copy_count = std::min(*pPropertyCount, static_cast<uint32_t>(combined_extensions.size()));
    for (uint32_t i = 0; i < copy_count; i++) {
        pProperties[i] = combined_extensions[i];
    }

    *pPropertyCount = copy_count;
    return copy_count < combined_extensions.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL internal_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties)
{
    using namespace mali_wrapper;

    if (pPropertyCount == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    auto mali_enumerate = get_mali_instance_proc_for_physical_device<PFN_vkEnumerateDeviceExtensionProperties>(
        physicalDevice, "vkEnumerateDeviceExtensionProperties");
    if (mali_enumerate == nullptr) {
        mali_enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            LibraryLoader::Instance().GetMaliProcAddr("vkEnumerateDeviceExtensionProperties"));
    }

    if (mali_enumerate == nullptr) {
        *pPropertyCount = 0;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (pLayerName != nullptr || !should_filter_external_memory_host_extension()) {
        return mali_enumerate(physicalDevice, pLayerName, pPropertyCount, pProperties);
    }

    uint32_t mali_count = 0;
    VkResult result = mali_enumerate(physicalDevice, pLayerName, &mali_count, nullptr);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return result;
    }

    std::vector<VkExtensionProperties> mali_extensions(mali_count);
    if (mali_count > 0) {
        result = mali_enumerate(physicalDevice, pLayerName, &mali_count, mali_extensions.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            return result;
        }
        mali_extensions.resize(mali_count);
    }

    std::vector<VkExtensionProperties> filtered_extensions;
    filtered_extensions.reserve(mali_extensions.size());
    for (const auto& extension : mali_extensions) {
        if (!is_filtered_device_extension(extension.extensionName)) {
            filtered_extensions.push_back(extension);
        }
    }

    if (pProperties == nullptr) {
        *pPropertyCount = static_cast<uint32_t>(filtered_extensions.size());
        return VK_SUCCESS;
    }

    const uint32_t copy_count = std::min(*pPropertyCount, static_cast<uint32_t>(filtered_extensions.size()));
    for (uint32_t i = 0; i < copy_count; i++) {
        pProperties[i] = filtered_extensions[i];
    }

    *pPropertyCount = copy_count;
    return copy_count < filtered_extensions.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL internal_vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    using namespace mali_wrapper;

    if (!pName) {
        return nullptr;
    }


    if (strcmp(pName, "vkGetInstanceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkGetInstanceProcAddr);
    }

    if (strcmp(pName, "vkCreateInstance") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkCreateInstance);
    }

    if (strcmp(pName, "vkDestroyInstance") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkDestroyInstance);
    }

    if (strcmp(pName, "vkDestroyDevice") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkDestroyDevice);
    }

    if (strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkEnumerateInstanceExtensionProperties);
    }

    if (strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkEnumerateDeviceExtensionProperties);
    }

    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkGetDeviceProcAddr);
    }

    if (strcmp(pName, "vkCreateDevice") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkCreateDevice);
    }

    if (strcmp(pName, "vkGetPhysicalDeviceFeatures") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkGetPhysicalDeviceFeatures);
    }

    if (strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkGetPhysicalDeviceFeatures2);
    }

    if (strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkGetPhysicalDeviceFeatures2KHR);
    }

    if (GetWSIManager().is_wsi_function(pName)) {
        auto func = GetWSIManager().get_function_pointer(pName);
        if (func) {
            return func;
        }
    }

    auto mali_proc_addr = LibraryLoader::Instance().GetMaliGetInstanceProcAddr();
    if (mali_proc_addr) {
        VkInstance mali_instance = instance;
        if (!mali_instance) {
            std::lock_guard<std::mutex> lock(instance_mutex);
            mali_instance = !managed_instances.empty() ? managed_instances.begin()->first : VK_NULL_HANDLE;
        }

        auto func = mali_proc_addr(mali_instance, pName);
        if (func) {
            return func;
        }
    }

    return nullptr;
}

static VKAPI_ATTR void VKAPI_CALL internal_vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures* pFeatures)
{
    using namespace mali_wrapper;

    if (pFeatures == nullptr) {
        return;
    }

    auto mali_get_features = get_mali_instance_proc_for_physical_device<PFN_vkGetPhysicalDeviceFeatures>(
        physicalDevice, "vkGetPhysicalDeviceFeatures");
    if (mali_get_features != nullptr) {
        mali_get_features(physicalDevice, pFeatures);
    } else {
        std::memset(pFeatures, 0, sizeof(*pFeatures));
    }

    advertise_spoofed_physical_features(pFeatures);
}

static VKAPI_ATTR void VKAPI_CALL internal_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures)
{
    using namespace mali_wrapper;

    if (pFeatures == nullptr) {
        return;
    }

    auto mali_get_features2 = get_mali_instance_proc_for_physical_device<PFN_vkGetPhysicalDeviceFeatures2>(
        physicalDevice, "vkGetPhysicalDeviceFeatures2");
    if (mali_get_features2 != nullptr) {
        mali_get_features2(physicalDevice, pFeatures);
    } else {
        auto mali_get_features2_khr =
            get_mali_instance_proc_for_physical_device<PFN_vkGetPhysicalDeviceFeatures2KHR>(
                physicalDevice, "vkGetPhysicalDeviceFeatures2KHR");
        if (mali_get_features2_khr != nullptr) {
            mali_get_features2_khr(physicalDevice, reinterpret_cast<VkPhysicalDeviceFeatures2KHR*>(pFeatures));
        } else {
            std::memset(&pFeatures->features, 0, sizeof(pFeatures->features));
            internal_vkGetPhysicalDeviceFeatures(physicalDevice, &pFeatures->features);
        }
    }

    advertise_spoofed_physical_features(&pFeatures->features);
    advertise_spoofed_physical_feature_chain(pFeatures->pNext);
}

static VKAPI_ATTR void VKAPI_CALL internal_vkGetPhysicalDeviceFeatures2KHR(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2KHR* pFeatures)
{
    internal_vkGetPhysicalDeviceFeatures2(physicalDevice, reinterpret_cast<VkPhysicalDeviceFeatures2*>(pFeatures));
}

template <typename T>
static T get_mali_device_proc(VkDevice device, const char* proc_name)
{
    using namespace mali_wrapper;

    auto mali_proc_addr = LibraryLoader::Instance().GetMaliGetInstanceProcAddr();
    if (!mali_proc_addr) {
        return nullptr;
    }

    VkInstance parent_instance = get_device_parent_instance(device);
    if (parent_instance == VK_NULL_HANDLE) {
        return nullptr;
    }

    auto mali_get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        mali_proc_addr(parent_instance, "vkGetDeviceProcAddr"));
    if (!mali_get_device_proc_addr) {
        return nullptr;
    }

    return reinterpret_cast<T>(mali_get_device_proc_addr(device, proc_name));
}

static void maybe_apply_shadow_mapping(VkDevice device, VkDeviceMemory memory,
                                       VkDeviceSize offset, VkDeviceSize size, void** ppData)
{
    using namespace mali_wrapper;

    if (ppData == nullptr || *ppData == nullptr) {
        return;
    }

    if (should_collect_low_address_map_stats()) {
        low_address_map_stats.successful_maps.fetch_add(1, std::memory_order_relaxed);
    }

    if (is_pointer_32bit_compatible(*ppData)) {
        if (should_collect_low_address_map_stats()) {
            low_address_map_stats.compatible_maps.fetch_add(1, std::memory_order_relaxed);
        }
        if (should_trace_low_address_map_events()) {
            LOW_ADDRESS_LOG_DEBUG("Low-address map bypassed: pointer already 32-bit-compatible, ptr=" +
                                  format_pointer(*ppData) +
                                  ", offset=" + std::to_string(offset) +
                                  ", size=" + std::to_string(size));
        }
        maybe_log_low_address_map_progress("already-32bit", false);
        return;
    }

    if (should_collect_low_address_map_stats()) {
        low_address_map_stats.high_pointer_maps.fetch_add(1, std::memory_order_relaxed);
    }

    if (should_trace_low_address_map_events()) {
        LOW_ADDRESS_LOG_DEBUG("Low-address high pointer detected: real=" + format_pointer(*ppData) +
                              ", offset=" + std::to_string(offset) +
                              ", size=" + std::to_string(size));
    }

    if (!should_use_low_address_shadow_map()) {
        if (should_collect_low_address_map_stats()) {
            low_address_map_stats.disabled_high_pointer_maps.fetch_add(1, std::memory_order_relaxed);
        }
        static std::once_flag warning_once;
        std::call_once(warning_once, []() {
            LOW_ADDRESS_LOG_WARN("Detected >32-bit mapped pointer while low-address workaround is disabled. "
                                 "Set MALI_WRAPPER_LOW_ADDRESS_MAP=1 to force compatibility mode.");
        });
        maybe_log_low_address_map_progress("disabled-high-pointer", true);
        return;
    }

    const DeviceMemoryKey key = make_memory_key(device, memory);

    VkDeviceSize resolved_size = 0;
    {
        std::lock_guard<std::mutex> lock(memory_tracking_mutex);
        if (!resolve_map_size_locked(key, offset, size, &resolved_size)) {
            if (should_collect_low_address_map_stats()) {
                low_address_map_stats.resolve_failures.fetch_add(1, std::memory_order_relaxed);
            }
            LOW_ADDRESS_LOG_WARN("Low-address map workaround skipped: unable to resolve mapping size");
            maybe_log_low_address_map_progress("resolve-failed", true);
            return;
        }
    }

    if (resolved_size == 0 || resolved_size > static_cast<VkDeviceSize>(std::numeric_limits<size_t>::max())) {
        if (should_collect_low_address_map_stats()) {
            low_address_map_stats.unsupported_size_failures.fetch_add(1, std::memory_order_relaxed);
        }
        LOW_ADDRESS_LOG_WARN("Low-address map workaround skipped: mapping size is unsupported");
        maybe_log_low_address_map_progress("size-unsupported", true);
        return;
    }

    const void* real_ptr = *ppData;
    const ShadowAllocationResult allocation =
        allocate_low_address_shadow(static_cast<size_t>(resolved_size));
    if (allocation.ptr == nullptr) {
        if (should_collect_low_address_map_stats()) {
            low_address_map_stats.allocation_failures.fetch_add(1, std::memory_order_relaxed);
            low_address_map_stats.allocation_time_ns.fetch_add(allocation.elapsed_ns, std::memory_order_relaxed);
            low_address_map_stats.fixed_search_attempts.fetch_add(
                allocation.fixed_search_attempts, std::memory_order_relaxed);
        }
        LOW_ADDRESS_LOG_WARN("Low-address map workaround failed: unable to allocate shadow mapping");
        if (should_trace_low_address_map_events()) {
            LOW_ADDRESS_LOG_INFO("Low-address shadow map allocation failure: real=" + format_pointer(real_ptr) +
                                 ", size=" + format_bytes(static_cast<uint64_t>(resolved_size)) +
                                 ", last_errno=" + std::to_string(allocation.last_errno) +
                                 ", fixed_search_attempts=" + std::to_string(allocation.fixed_search_attempts));
        }
        maybe_log_low_address_map_progress("alloc-failed", true);
        return;
    }

    tracked_memcpy(allocation.ptr, real_ptr, static_cast<size_t>(resolved_size),
                   LowAddressCopyKind::MAP_TO_SHADOW);

    ShadowMappingInfo stale_mapping{};
    bool has_stale_mapping = false;
    {
        std::lock_guard<std::mutex> lock(memory_tracking_mutex);
        auto it = shadow_mappings.find(key);
        if (it != shadow_mappings.end()) {
            stale_mapping = it->second;
            has_stale_mapping = true;
            it->second = ShadowMappingInfo{ const_cast<void*>(real_ptr), allocation.ptr,
                                            allocation.size, offset, resolved_size };
        } else {
            shadow_mappings.emplace(key, ShadowMappingInfo{ const_cast<void*>(real_ptr), allocation.ptr,
                                                            allocation.size, offset, resolved_size });
        }
    }

    record_shadow_mapping_installed(allocation.size,
                                    has_stale_mapping ? stale_mapping.shadow_size : 0,
                                    allocation);

    if (has_stale_mapping && stale_mapping.shadow_ptr != nullptr && stale_mapping.shadow_size > 0) {
        if (should_trace_low_address_map_events()) {
            LOW_ADDRESS_LOG_DEBUG("Low-address shadow map replaced: old_shadow=" +
                                  format_pointer(stale_mapping.shadow_ptr) +
                                  ", old_size=" + format_bytes(static_cast<uint64_t>(stale_mapping.shadow_size)));
        }
        record_shadow_mapping_removed(stale_mapping.shadow_size);
        munmap(stale_mapping.shadow_ptr, stale_mapping.shadow_size);
    }

    if (should_trace_low_address_map_events()) {
        LOW_ADDRESS_LOG_INFO("Low-address shadow map applied: real=" + format_pointer(real_ptr) +
                             ", shadow=" + format_pointer(allocation.ptr) +
                             ", size=" + format_bytes(static_cast<uint64_t>(resolved_size)) +
                             ", offset=" + std::to_string(offset) +
                             ", method=" + shadow_allocation_method_to_string(allocation.method) +
                             ", fixed_search_attempts=" + std::to_string(allocation.fixed_search_attempts) +
                             ", allocation_time=" + format_duration_ms(allocation.elapsed_ns));
    }

    maybe_log_low_address_map_progress("shadow-applied", true);

    *ppData = allocation.ptr;
}

static void sync_shadow_to_real(VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange* pMemoryRanges)
{
    using namespace mali_wrapper;

    if (memoryRangeCount == 0 || pMemoryRanges == nullptr) {
        return;
    }

    bool copied_anything = false;
    std::lock_guard<std::mutex> lock(memory_tracking_mutex);
    for (uint32_t i = 0; i < memoryRangeCount; i++) {
        const VkMappedMemoryRange& range = pMemoryRanges[i];
        const DeviceMemoryKey key = make_memory_key(device, range.memory);
        auto map_it = shadow_mappings.find(key);
        if (map_it == shadow_mappings.end()) {
            continue;
        }

        size_t byte_offset = 0;
        size_t byte_count = 0;
        if (!compute_copy_region(map_it->second, range.offset, range.size, &byte_offset, &byte_count)) {
            continue;
        }

        auto* shadow_bytes = static_cast<const uint8_t*>(map_it->second.shadow_ptr);
        auto* real_bytes = static_cast<uint8_t*>(map_it->second.real_ptr);
        tracked_memcpy(real_bytes + byte_offset, shadow_bytes + byte_offset, byte_count,
                       LowAddressCopyKind::FLUSH_TO_REAL);
        copied_anything = true;
    }

    if (copied_anything) {
        maybe_log_low_address_map_progress("flush", false);
    }
}

static void sync_real_to_shadow(VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange* pMemoryRanges)
{
    using namespace mali_wrapper;

    if (memoryRangeCount == 0 || pMemoryRanges == nullptr) {
        return;
    }

    bool copied_anything = false;
    std::lock_guard<std::mutex> lock(memory_tracking_mutex);
    for (uint32_t i = 0; i < memoryRangeCount; i++) {
        const VkMappedMemoryRange& range = pMemoryRanges[i];
        const DeviceMemoryKey key = make_memory_key(device, range.memory);
        auto map_it = shadow_mappings.find(key);
        if (map_it == shadow_mappings.end()) {
            continue;
        }

        size_t byte_offset = 0;
        size_t byte_count = 0;
        if (!compute_copy_region(map_it->second, range.offset, range.size, &byte_offset, &byte_count)) {
            continue;
        }

        auto* shadow_bytes = static_cast<uint8_t*>(map_it->second.shadow_ptr);
        auto* real_bytes = static_cast<const uint8_t*>(map_it->second.real_ptr);
        tracked_memcpy(shadow_bytes + byte_offset, real_bytes + byte_offset, byte_count,
                       LowAddressCopyKind::INVALIDATE_TO_SHADOW);
        copied_anything = true;
    }

    if (copied_anything) {
        maybe_log_low_address_map_progress("invalidate", false);
    }
}

static void sync_all_shadows_for_device(VkDevice device)
{
    using namespace mali_wrapper;

    if (device == VK_NULL_HANDLE) {
        return;
    }

    bool copied_anything = false;
    std::lock_guard<std::mutex> lock(memory_tracking_mutex);
    for (auto& entry : shadow_mappings) {
        if (entry.first.device != device) {
            continue;
        }

        auto& mapping = entry.second;
        if (mapping.real_ptr == nullptr || mapping.shadow_ptr == nullptr ||
            mapping.mapped_size == 0 ||
            mapping.mapped_size > static_cast<VkDeviceSize>(std::numeric_limits<size_t>::max())) {
            continue;
        }

        tracked_memcpy(mapping.real_ptr, mapping.shadow_ptr, static_cast<size_t>(mapping.mapped_size),
                       LowAddressCopyKind::SUBMIT_TO_REAL);
        copied_anything = true;
    }

    if (copied_anything) {
        maybe_log_low_address_map_progress("queue-submit", false);
    }
}

static void sync_all_shadows()
{
    using namespace mali_wrapper;

    bool copied_anything = false;
    std::lock_guard<std::mutex> lock(memory_tracking_mutex);
    for (auto& entry : shadow_mappings) {
        auto& mapping = entry.second;
        if (mapping.real_ptr == nullptr || mapping.shadow_ptr == nullptr ||
            mapping.mapped_size == 0 ||
            mapping.mapped_size > static_cast<VkDeviceSize>(std::numeric_limits<size_t>::max())) {
            continue;
        }

        tracked_memcpy(mapping.real_ptr, mapping.shadow_ptr, static_cast<size_t>(mapping.mapped_size),
                       LowAddressCopyKind::SUBMIT_TO_REAL);
        copied_anything = true;
    }

    if (copied_anything) {
        maybe_log_low_address_map_progress("queue-submit", false);
    }
}

static VkDevice get_queue_parent_device_safe(VkQueue queue)
{
    if (queue == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }

    try {
        auto& queue_device_data = mali_wrapper::device_private_data::get(queue);
        return queue_device_data.device;
    } catch (...) {
        return VK_NULL_HANDLE;
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL internal_vkAllocateMemory(
    VkDevice device,
    const VkMemoryAllocateInfo* pAllocateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDeviceMemory* pMemory)
{
    using namespace mali_wrapper;

    auto mali_allocate_memory = get_mali_device_proc<PFN_vkAllocateMemory>(device, "vkAllocateMemory");
    if (!mali_allocate_memory) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const VkResult result = mali_allocate_memory(device, pAllocateInfo, pAllocator, pMemory);
    if (result == VK_SUCCESS && pMemory != nullptr && *pMemory != VK_NULL_HANDLE && pAllocateInfo != nullptr) {
        std::lock_guard<std::mutex> lock(memory_tracking_mutex);
        tracked_memory_allocations[make_memory_key(device, *pMemory)] = pAllocateInfo->allocationSize;
    }

    return result;
}

static VKAPI_ATTR void VKAPI_CALL internal_vkFreeMemory(
    VkDevice device,
    VkDeviceMemory memory,
    const VkAllocationCallbacks* pAllocator)
{
    using namespace mali_wrapper;

    ShadowMappingInfo stale_mapping{};
    bool has_stale_mapping = false;

    {
        std::lock_guard<std::mutex> lock(memory_tracking_mutex);
        const DeviceMemoryKey key = make_memory_key(device, memory);
        tracked_memory_allocations.erase(key);

        auto mapping_it = shadow_mappings.find(key);
        if (mapping_it != shadow_mappings.end()) {
            stale_mapping = mapping_it->second;
            has_stale_mapping = true;
            shadow_mappings.erase(mapping_it);
        }
    }

    if (has_stale_mapping && stale_mapping.shadow_ptr != nullptr && stale_mapping.shadow_size > 0) {
        LOW_ADDRESS_LOG_INFO("Low-address free cleanup: memory=" +
                             format_device_memory_handle(memory) +
                             ", shadow=" + format_pointer(stale_mapping.shadow_ptr) +
                             ", size=" + format_bytes(static_cast<uint64_t>(stale_mapping.shadow_size)));
        record_shadow_mapping_removed(stale_mapping.shadow_size);
        munmap(stale_mapping.shadow_ptr, stale_mapping.shadow_size);
    }

    auto mali_free_memory = get_mali_device_proc<PFN_vkFreeMemory>(device, "vkFreeMemory");
    if (mali_free_memory) {
        mali_free_memory(device, memory, pAllocator);
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL internal_vkMapMemory(
    VkDevice device,
    VkDeviceMemory memory,
    VkDeviceSize offset,
    VkDeviceSize size,
    VkMemoryMapFlags flags,
    void** ppData)
{
    auto mali_map_memory = get_mali_device_proc<PFN_vkMapMemory>(device, "vkMapMemory");
    if (!mali_map_memory) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult result = mali_map_memory(device, memory, offset, size, flags, ppData);
    if (result == VK_SUCCESS) {
        maybe_apply_shadow_mapping(device, memory, offset, size, ppData);
    }

    return result;
}

static bool pop_shadow_mapping(VkDevice device, VkDeviceMemory memory, mali_wrapper::ShadowMappingInfo* out_mapping)
{
    using namespace mali_wrapper;

    if (out_mapping == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(memory_tracking_mutex);
    const DeviceMemoryKey key = make_memory_key(device, memory);
    auto mapping_it = shadow_mappings.find(key);
    if (mapping_it == shadow_mappings.end()) {
        return false;
    }

    *out_mapping = mapping_it->second;
    shadow_mappings.erase(mapping_it);
    return true;
}

static void finalize_shadow_mapping(mali_wrapper::ShadowMappingInfo& mapping)
{
    using namespace mali_wrapper;

    if (mapping.real_ptr != nullptr && mapping.shadow_ptr != nullptr &&
        mapping.mapped_size > 0 &&
        mapping.mapped_size <= static_cast<VkDeviceSize>(std::numeric_limits<size_t>::max())) {
        tracked_memcpy(mapping.real_ptr, mapping.shadow_ptr, static_cast<size_t>(mapping.mapped_size),
                       LowAddressCopyKind::UNMAP_TO_REAL);
    }

    if (mapping.shadow_ptr != nullptr && mapping.shadow_size > 0) {
        if (should_trace_low_address_map_events()) {
            LOW_ADDRESS_LOG_INFO("Low-address shadow map finalized: real=" + format_pointer(mapping.real_ptr) +
                                 ", shadow=" + format_pointer(mapping.shadow_ptr) +
                                 ", size=" + format_bytes(static_cast<uint64_t>(mapping.mapped_size)));
        }
        record_shadow_mapping_removed(mapping.shadow_size);
        munmap(mapping.shadow_ptr, mapping.shadow_size);
        maybe_log_low_address_map_progress("unmap", true);
    }
}

static VKAPI_ATTR void VKAPI_CALL internal_vkUnmapMemory(
    VkDevice device,
    VkDeviceMemory memory)
{
    mali_wrapper::ShadowMappingInfo mapping{};
    if (pop_shadow_mapping(device, memory, &mapping)) {
        finalize_shadow_mapping(mapping);
    }

    auto mali_unmap_memory = get_mali_device_proc<PFN_vkUnmapMemory>(device, "vkUnmapMemory");
    if (mali_unmap_memory) {
        mali_unmap_memory(device, memory);
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL internal_vkFlushMappedMemoryRanges(
    VkDevice device,
    uint32_t memoryRangeCount,
    const VkMappedMemoryRange* pMemoryRanges)
{
    auto mali_flush = get_mali_device_proc<PFN_vkFlushMappedMemoryRanges>(device, "vkFlushMappedMemoryRanges");
    if (!mali_flush) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    sync_shadow_to_real(device, memoryRangeCount, pMemoryRanges);
    return mali_flush(device, memoryRangeCount, pMemoryRanges);
}

static VKAPI_ATTR VkResult VKAPI_CALL internal_vkInvalidateMappedMemoryRanges(
    VkDevice device,
    uint32_t memoryRangeCount,
    const VkMappedMemoryRange* pMemoryRanges)
{
    auto mali_invalidate = get_mali_device_proc<PFN_vkInvalidateMappedMemoryRanges>(device, "vkInvalidateMappedMemoryRanges");
    if (!mali_invalidate) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const VkResult result = mali_invalidate(device, memoryRangeCount, pMemoryRanges);
    if (result == VK_SUCCESS) {
        sync_real_to_shadow(device, memoryRangeCount, pMemoryRanges);
    }
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL internal_vkMapMemory2KHR(
    VkDevice device,
    const VkMemoryMapInfoKHR* pMemoryMapInfo,
    void** ppData)
{
    if (pMemoryMapInfo == nullptr || ppData == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    auto mali_map_memory2 = get_mali_device_proc<PFN_vkMapMemory2KHR>(device, "vkMapMemory2KHR");
    if (!mali_map_memory2) {
        mali_map_memory2 = reinterpret_cast<PFN_vkMapMemory2KHR>(
            get_mali_device_proc<PFN_vkVoidFunction>(device, "vkMapMemory2"));
    }
    if (!mali_map_memory2) {
        // Some drivers do not expose VK_KHR_map_memory2 even though vkMapMemory works.
        return internal_vkMapMemory(device, pMemoryMapInfo->memory, pMemoryMapInfo->offset,
                                    pMemoryMapInfo->size, pMemoryMapInfo->flags, ppData);
    }

    VkResult result = mali_map_memory2(device, pMemoryMapInfo, ppData);
    if (result == VK_SUCCESS) {
        maybe_apply_shadow_mapping(device, pMemoryMapInfo->memory, pMemoryMapInfo->offset, pMemoryMapInfo->size, ppData);
    }
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL internal_vkUnmapMemory2KHR(
    VkDevice device,
    const VkMemoryUnmapInfoKHR* pMemoryUnmapInfo)
{
    if (pMemoryUnmapInfo != nullptr) {
        mali_wrapper::ShadowMappingInfo mapping{};
        if (pop_shadow_mapping(device, pMemoryUnmapInfo->memory, &mapping)) {
            finalize_shadow_mapping(mapping);
        }
    }

    auto mali_unmap2 = get_mali_device_proc<PFN_vkUnmapMemory2KHR>(device, "vkUnmapMemory2KHR");
    if (!mali_unmap2) {
        mali_unmap2 = reinterpret_cast<PFN_vkUnmapMemory2KHR>(
            get_mali_device_proc<PFN_vkVoidFunction>(device, "vkUnmapMemory2"));
    }
    if (!mali_unmap2) {
        return VK_SUCCESS;
    }

    return mali_unmap2(device, pMemoryUnmapInfo);
}

static VKAPI_ATTR VkResult VKAPI_CALL internal_vkQueueSubmit(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence fence)
{
    VkDevice device = get_queue_parent_device_safe(queue);
    if (device != VK_NULL_HANDLE) {
        sync_all_shadows_for_device(device);
    } else {
        sync_all_shadows();
    }

    auto mali_queue_submit = (device != VK_NULL_HANDLE)
                                 ? get_mali_device_proc<PFN_vkQueueSubmit>(device, "vkQueueSubmit")
                                 : nullptr;
    if (!mali_queue_submit) {
        const VkDevice fallback_device = mali_wrapper::get_any_managed_device();
        if (fallback_device != VK_NULL_HANDLE) {
            mali_queue_submit = get_mali_device_proc<PFN_vkQueueSubmit>(fallback_device, "vkQueueSubmit");
        }
    }
    if (!mali_queue_submit) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    return mali_queue_submit(queue, submitCount, pSubmits, fence);
}

static VKAPI_ATTR VkResult VKAPI_CALL internal_vkQueueSubmit2(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo2* pSubmits,
    VkFence fence)
{
    VkDevice device = get_queue_parent_device_safe(queue);
    if (device != VK_NULL_HANDLE) {
        sync_all_shadows_for_device(device);
    } else {
        sync_all_shadows();
        device = mali_wrapper::get_any_managed_device();
    }

    if (device == VK_NULL_HANDLE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    auto mali_queue_submit2 = get_mali_device_proc<PFN_vkQueueSubmit2>(device, "vkQueueSubmit2");
    if (mali_queue_submit2) {
        return mali_queue_submit2(queue, submitCount, pSubmits, fence);
    }

    auto mali_queue_submit2_khr = get_mali_device_proc<PFN_vkQueueSubmit2KHR>(device, "vkQueueSubmit2KHR");
    if (!mali_queue_submit2_khr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    return mali_queue_submit2_khr(queue, submitCount, reinterpret_cast<const VkSubmitInfo2KHR*>(pSubmits), fence);
}

static VKAPI_ATTR VkResult VKAPI_CALL internal_vkQueueSubmit2KHR(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo2KHR* pSubmits,
    VkFence fence)
{
    VkDevice device = get_queue_parent_device_safe(queue);
    if (device != VK_NULL_HANDLE) {
        sync_all_shadows_for_device(device);
    } else {
        sync_all_shadows();
        device = mali_wrapper::get_any_managed_device();
    }

    if (device == VK_NULL_HANDLE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    auto mali_queue_submit2_khr = get_mali_device_proc<PFN_vkQueueSubmit2KHR>(device, "vkQueueSubmit2KHR");
    if (mali_queue_submit2_khr) {
        return mali_queue_submit2_khr(queue, submitCount, pSubmits, fence);
    }

    auto mali_queue_submit2 = get_mali_device_proc<PFN_vkQueueSubmit2>(device, "vkQueueSubmit2");
    if (!mali_queue_submit2) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    return mali_queue_submit2(queue, submitCount, reinterpret_cast<const VkSubmitInfo2*>(pSubmits), fence);
}

static VKAPI_ATTR VkResult VKAPI_CALL internal_vkCreateImage(
    VkDevice device,
    const VkImageCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImage* pImage)
{
    auto mali_create_image = get_mali_device_proc<PFN_vkCreateImage>(device, "vkCreateImage");
    if (mali_create_image == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    return mali_create_image(device, pCreateInfo, pAllocator, pImage);
}

static VKAPI_ATTR VkResult VKAPI_CALL internal_vkCreateGraphicsPipelines(
    VkDevice device,
    VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines)
{
    auto mali_create_graphics_pipelines =
        get_mali_device_proc<PFN_vkCreateGraphicsPipelines>(device, "vkCreateGraphicsPipelines");
    if (mali_create_graphics_pipelines == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    bool trapped_signal = false;
    return mali_wrapper::call_vkCreateGraphicsPipelines_with_signal_guard(
        mali_create_graphics_pipelines,
        device,
        pipelineCache,
        createInfoCount,
        pCreateInfos,
        pAllocator,
        pPipelines,
        &trapped_signal);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL internal_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    using namespace mali_wrapper;

    if (!pName) {
        return nullptr;
    }

    uintptr_t device_ptr = reinterpret_cast<uintptr_t>(device);
    char hex_buffer[32];
    snprintf(hex_buffer, sizeof(hex_buffer), "0x%lx", device_ptr);

    if (strcmp(pName, "vkDestroyDevice") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkDestroyDevice);
    }

    if (strcmp(pName, "vkAllocateMemory") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkAllocateMemory);
    }
    if (strcmp(pName, "vkCreateImage") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkCreateImage);
    }
    if (strcmp(pName, "vkCreateGraphicsPipelines") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkCreateGraphicsPipelines);
    }
    if (strcmp(pName, "vkFreeMemory") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkFreeMemory);
    }
    if (strcmp(pName, "vkMapMemory") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkMapMemory);
    }
    if (strcmp(pName, "vkUnmapMemory") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkUnmapMemory);
    }
    if (strcmp(pName, "vkFlushMappedMemoryRanges") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkFlushMappedMemoryRanges);
    }
    if (strcmp(pName, "vkInvalidateMappedMemoryRanges") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkInvalidateMappedMemoryRanges);
    }
    if (strcmp(pName, "vkQueueSubmit") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkQueueSubmit);
    }
    if (strcmp(pName, "vkQueueSubmit2") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkQueueSubmit2);
    }
    if (strcmp(pName, "vkQueueSubmit2KHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkQueueSubmit2KHR);
    }
    if (strcmp(pName, "vkMapMemory2KHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkMapMemory2KHR);
    }
    if (strcmp(pName, "vkMapMemory2") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkMapMemory2KHR);
    }
    if (strcmp(pName, "vkUnmapMemory2KHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkUnmapMemory2KHR);
    }
    if (strcmp(pName, "vkUnmapMemory2") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkUnmapMemory2KHR);
    }

    if (GetWSIManager().is_wsi_function(pName)) {
        auto func = GetWSIManager().get_function_pointer(pName);
        if (func) {
            return func;
        } else {
            return nullptr;
        }
    }

    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkGetDeviceProcAddr);
    }

    if (strstr(pName, "RayTracing") || strstr(pName, "MeshTask")) {
        return nullptr;
    }

    auto mali_proc_addr = LibraryLoader::Instance().GetMaliGetInstanceProcAddr();
    if (mali_proc_addr) {
        VkInstance parent_instance = get_device_parent_instance(device);
        if (parent_instance != VK_NULL_HANDLE) {
            auto mali_get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                mali_proc_addr(parent_instance, "vkGetDeviceProcAddr"));
            if (mali_get_device_proc_addr) {
                auto func = mali_get_device_proc_addr(device, pName);
                if (func) {
                    return func;
                }
            }
        }
    }

    return nullptr;
}

static VKAPI_ATTR VkResult VKAPI_CALL wrapper_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pSwapchainCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
    using namespace mali_wrapper;

    uintptr_t device_ptr = reinterpret_cast<uintptr_t>(device);
    char hex_buffer[32];
    snprintf(hex_buffer, sizeof(hex_buffer), "0x%lx", device_ptr);


    void* device_key = nullptr;
    if (device != VK_NULL_HANDLE) {
        device_key = *reinterpret_cast<void**>(device);
        char key_hex[32];
        snprintf(key_hex, sizeof(key_hex), "0x%lx", reinterpret_cast<uintptr_t>(device_key));
    }

    void* wsi_lib = LibraryLoader::Instance().GetWSILibraryHandle();
    if (!wsi_lib) {
        LOG_ERROR("WSI layer library not available");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    auto wsi_create_swapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
        dlsym(wsi_lib, "wsi_layer_vkCreateSwapchainKHR"));
    if (!wsi_create_swapchain) {
        LOG_ERROR("WSI layer vkCreateSwapchainKHR function not found");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult result = wsi_create_swapchain(device, pSwapchainCreateInfo, pAllocator, pSwapchain);

    return result;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL filtered_mali_get_device_proc_addr(VkDevice device, const char* pName) {
    using namespace mali_wrapper;

    if (!pName) {
        return nullptr;
    }

    if (IsWSIFunction(pName)) {
        return nullptr;
    }

    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkGetDeviceProcAddr);
    }

    auto mali_proc_addr = LibraryLoader::Instance().GetMaliGetInstanceProcAddr();
    if (mali_proc_addr) {
        VkInstance parent_instance = get_device_parent_instance(device);
        if (parent_instance != VK_NULL_HANDLE) {
            auto mali_get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                mali_proc_addr(parent_instance, "vkGetDeviceProcAddr"));
            if (mali_get_device_proc_addr) {
                auto func = mali_get_device_proc_addr(device, pName);
                if (func) {
                    return func;
                } else {
                    return nullptr;
                }
            } else {
                return nullptr;
            }
        } else {
            return nullptr;
        }
    } else {
        return nullptr;
    }

    return nullptr;
}

static VKAPI_ATTR VkResult VKAPI_CALL dummy_set_device_loader_data(VkDevice device, void* object) {
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL internal_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice) {

    using namespace mali_wrapper;


    if (!pCreateInfo || !pDevice) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    std::vector<const char *> enabled_extensions;
    std::unique_ptr<util::extension_list> extension_list_ptr;

    try
    {
        auto &instance_data = instance_private_data::get(physicalDevice);
        util::allocator extension_allocator(instance_data.get_allocator(), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
        extension_list_ptr = std::make_unique<util::extension_list>(extension_allocator);
        auto &extensions = *extension_list_ptr;

        if (pCreateInfo->enabledExtensionCount > 0 && pCreateInfo->ppEnabledExtensionNames != nullptr)
        {
            extensions.add(pCreateInfo->ppEnabledExtensionNames, pCreateInfo->enabledExtensionCount);
        }

        VkResult extension_result = wsi::add_device_extensions_required_by_layer(
            physicalDevice, instance_data.get_enabled_platforms(), extensions);
        if (extension_result != VK_SUCCESS)
        {
            LOG_ERROR("Failed to collect WSI-required device extensions, error: " +
                      std::to_string(extension_result));
            return extension_result;
        }

        util::vector<const char *> extension_vector(extension_allocator);
        extensions.get_extension_strings(extension_vector);

        std::unordered_set<std::string> seen_extensions;
        seen_extensions.reserve(extension_vector.size());

        for (const char *name : extension_vector)
        {
            if (name == nullptr)
            {
                continue;
            }
            auto inserted = seen_extensions.emplace(name);
            if (inserted.second)
            {
                enabled_extensions.push_back(name);
            }
        }
    }
    catch (const std::exception &e)
    {
        LOG_WARN(std::string("Unable to augment device extensions: ") + e.what());
        enabled_extensions.clear();
        extension_list_ptr.reset();
    }

    const char *const *extension_name_ptr = pCreateInfo->ppEnabledExtensionNames;
    size_t extension_name_count = pCreateInfo->enabledExtensionCount;

    if (!enabled_extensions.empty())
    {
        extension_name_ptr = enabled_extensions.data();
        extension_name_count = enabled_extensions.size();
    }

    std::vector<const char*> filtered_requested_extensions;
    if (should_filter_external_memory_host_extension() &&
        extension_name_ptr != nullptr && extension_name_count > 0) {
        filtered_requested_extensions.assign(extension_name_ptr, extension_name_ptr + extension_name_count);
        const size_t removed_count = filter_device_extension_vector(&filtered_requested_extensions);
        if (removed_count > 0) {
            LOG_WARN("Removed " + std::to_string(removed_count) +
                     " filtered device extension(s) before vkCreateDevice");
            extension_name_ptr = filtered_requested_extensions.data();
            extension_name_count = filtered_requested_extensions.size();
        }
    }

    VkDeviceCreateInfo modified_create_info = *pCreateInfo;
    modified_create_info.enabledExtensionCount = static_cast<uint32_t>(extension_name_count);
    modified_create_info.ppEnabledExtensionNames = extension_name_ptr;
    DeviceCreateInfoFeatureSpoofCopies spoof_copies{};
    sanitize_device_create_info_for_driver(&modified_create_info, &spoof_copies);

    auto mali_proc_addr = LibraryLoader::Instance().GetMaliGetInstanceProcAddr();
    if (!mali_proc_addr) {
        LOG_ERROR("Mali driver not available for device creation");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (managed_instances.empty()) {
        LOG_ERROR("No managed instance available for Mali device creation");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    auto &wsinst = instance_private_data::get(physicalDevice);
    VkInstance mali_instance = wsinst.get_instance_handle();

    auto mali_create_device = reinterpret_cast<PFN_vkCreateDevice>(
        mali_proc_addr(mali_instance, "vkCreateDevice"));

    if (!mali_create_device) {
        LOG_ERROR("Mali driver vkCreateDevice not available");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult result = mali_create_device(physicalDevice, &modified_create_info, pAllocator, pDevice);

    if (result == VK_SUCCESS) {
        LOG_INFO("Device created successfully through Mali driver");

        managed_devices.emplace(*pDevice, mali_instance);

        VkInstance target_mali_instance = mali_instance;
        {
            std::lock_guard<std::mutex> lock(instance_mutex);
            if (latest_instance != VK_NULL_HANDLE) {
                auto latest_it = managed_instances.find(latest_instance);
                if (latest_it != managed_instances.end()) {
                    target_mali_instance = latest_instance;
                }
            }

            if (target_mali_instance == mali_instance && !managed_instances.empty()) {
                auto fallback = managed_instances.begin()->second.get();
                if (fallback != nullptr) {
                    target_mali_instance = fallback->instance;
                }
            }

            if (target_mali_instance != mali_instance) {
            } else if (!managed_instances.empty()) {
            } else {
            }
        }

        VkResult wsi_result = GetWSIManager().init_device(target_mali_instance, physicalDevice, *pDevice,
                                                         extension_name_ptr, extension_name_count);
        if (wsi_result != VK_SUCCESS) {
            LOG_ERROR("Failed to initialize WSI manager for device, error: " + std::to_string(wsi_result));
        } else {
            LOG_INFO("WSI manager initialized for device: " + std::to_string(reinterpret_cast<uintptr_t>(*pDevice)));
        }
    } else {
        LOG_ERROR("Failed to create device through Mali driver, error: " + std::to_string(result));
    }

    return result;
}

static VKAPI_ATTR void VKAPI_CALL internal_vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator) {

    using namespace mali_wrapper;


    if (device == VK_NULL_HANDLE) {
        return;
    }

    remove_tracking_for_device(device);

    VkInstance parent_instance = get_device_parent_instance(device);

    if (managed_devices.find(device) == managed_devices.end()) {
        LOG_WARN("Destroying unmanaged device");
    }

    GetWSIManager().release_device(device);

    auto mali_proc_addr = LibraryLoader::Instance().GetMaliGetInstanceProcAddr();
    PFN_vkDestroyDevice mali_destroy = nullptr;

    if (mali_proc_addr && parent_instance != VK_NULL_HANDLE) {
        auto mali_get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            mali_proc_addr(parent_instance, "vkGetDeviceProcAddr"));
        if (mali_get_device_proc_addr) {
            mali_destroy = reinterpret_cast<PFN_vkDestroyDevice>(
                mali_get_device_proc_addr(device, "vkDestroyDevice"));
        }
    }

    if (!mali_destroy) {
        mali_destroy = reinterpret_cast<PFN_vkDestroyDevice>(
            LibraryLoader::Instance().GetMaliProcAddr("vkDestroyDevice"));
    }

    if (mali_destroy) {
        mali_destroy(device, pAllocator);
        LOG_INFO("Device destroyed successfully");
    } else {
        LOG_WARN("Failed to locate Mali vkDestroyDevice entry point");
    }

    managed_devices.erase(device);
}

static VKAPI_ATTR VkResult VKAPI_CALL mali_driver_create_device(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice) {

    using namespace mali_wrapper;


    if (!pCreateInfo || !pDevice) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    std::vector<const char*> filtered_requested_extensions;
    const char* const* extension_name_ptr = pCreateInfo->ppEnabledExtensionNames;
    size_t extension_name_count = pCreateInfo->enabledExtensionCount;
    if (should_filter_external_memory_host_extension() &&
        extension_name_ptr != nullptr && extension_name_count > 0) {
        filtered_requested_extensions.assign(extension_name_ptr, extension_name_ptr + extension_name_count);
        const size_t removed_count = filter_device_extension_vector(&filtered_requested_extensions);
        if (removed_count > 0) {
            LOG_WARN("Removed " + std::to_string(removed_count) +
                     " filtered device extension(s) before Mali vkCreateDevice");
            extension_name_ptr = filtered_requested_extensions.data();
            extension_name_count = filtered_requested_extensions.size();
        }
    }

    VkDeviceCreateInfo modified_create_info = *pCreateInfo;
    modified_create_info.enabledExtensionCount = static_cast<uint32_t>(extension_name_count);
    modified_create_info.ppEnabledExtensionNames = extension_name_ptr;
    DeviceCreateInfoFeatureSpoofCopies spoof_copies{};
    sanitize_device_create_info_for_driver(&modified_create_info, &spoof_copies);

    auto mali_proc_addr = LibraryLoader::Instance().GetMaliGetInstanceProcAddr();
    if (!mali_proc_addr) {
        LOG_ERROR("Mali driver not available for device creation");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    auto mali_create_instance = LibraryLoader::Instance().GetMaliCreateInstance();
    if (!mali_create_instance) {
        LOG_ERROR("Mali driver vkCreateInstance not available");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkInstance mali_instance = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(instance_mutex);
        if (managed_instances.empty()) {
            LOG_ERROR("No managed instance available for Mali device creation");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (managed_instances.size() >= 2) {
            auto it = std::prev(managed_instances.end());
            mali_instance = it->first;
        } else {
            mali_instance = managed_instances.begin()->first;
        }
    }

    auto mali_create_device = reinterpret_cast<PFN_vkCreateDevice>(
        mali_proc_addr(mali_instance, "vkCreateDevice"));

    if (!mali_create_device) {
        mali_create_device = reinterpret_cast<PFN_vkCreateDevice>(
            LibraryLoader::Instance().GetMaliProcAddr("vkCreateDevice"));
    }

    if (!mali_create_device) {
        LOG_ERROR("Mali driver vkCreateDevice not available through any method");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult result = mali_create_device(physicalDevice, &modified_create_info, pAllocator, pDevice);

    if (result == VK_SUCCESS) {
        VkInstance target_mali_instance = mali_instance;
        {
            std::lock_guard<std::mutex> lock(instance_mutex);
            if (managed_instances.size() >= 2) {
                auto it = std::prev(managed_instances.end());
                target_mali_instance = it->first;  // This is the Mali instance, not application instance
            } else if (!managed_instances.empty()) {
                target_mali_instance = managed_instances.begin()->first;
            }
        }

        managed_devices.emplace(*pDevice, mali_instance);

        VkResult wsi_result = GetWSIManager().init_device(target_mali_instance, physicalDevice, *pDevice,
                                                         modified_create_info.ppEnabledExtensionNames,
                                                         modified_create_info.enabledExtensionCount);
        if (wsi_result != VK_SUCCESS) {
            LOG_ERROR("Failed to initialize WSI manager for device, error: " + std::to_string(wsi_result));
        } else {
        }
    } else {
        LOG_ERROR("Mali driver device creation failed, error: " + std::to_string(result));
    }

    return result;
}

extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName) {
    using namespace mali_wrapper;

    if (!pName) {
        return nullptr;
    }

    static bool initialized = false;
    if (!initialized) {
        if (!InitializeWrapper()) {
            return nullptr;
        }
        initialized = true;
    }

    return internal_vkGetInstanceProcAddr(instance, pName);
}

VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion) {
    if (pSupportedVersion) {
        *pSupportedVersion = 5;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return vk_icdGetInstanceProcAddr(instance, pName);
}

} // extern "C"
