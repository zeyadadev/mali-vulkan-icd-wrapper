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
#include <dlfcn.h>
#include <cstdio>
#include <memory>
#include <string>
#include <chrono>
#include <mutex>
#include <limits>
#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>

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

static std::mutex memory_tracking_mutex;
static std::unordered_map<DeviceMemoryKey, VkDeviceSize, DeviceMemoryKeyHash> tracked_memory_allocations;
static std::unordered_map<DeviceMemoryKey, ShadowMappingInfo, DeviceMemoryKeyHash> shadow_mappings;

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

static bool should_use_low_address_shadow_map()
{
    static int cached = -1;
    if (cached >= 0) {
        return cached == 1;
    }

    const bool forced = getenv("MALI_WRAPPER_LOW_ADDRESS_MAP") != nullptr;
    if (forced) {
        cached = is_bool_env_enabled("MALI_WRAPPER_LOW_ADDRESS_MAP", false) ? 1 : 0;
        return cached == 1;
    }

    const bool auto_enable = getenv("WINEWOW64") != nullptr || getenv("WINE_WOW64") != nullptr;
    cached = auto_enable ? 1 : 0;
    return cached == 1;
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

static void* allocate_low_address_shadow(size_t requested_size, size_t* out_shadow_size)
{
    if (out_shadow_size == nullptr || requested_size == 0) {
        return nullptr;
    }

    long page_size_value = sysconf(_SC_PAGESIZE);
    if (page_size_value <= 0) {
        page_size_value = 4096;
    }

    const size_t page_size = static_cast<size_t>(page_size_value);
    const size_t aligned_size = ((requested_size + page_size - 1) / page_size) * page_size;
    if (aligned_size == 0 || aligned_size < requested_size) {
        return nullptr;
    }

#ifdef MAP_32BIT
    void* mapped = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (mapped != MAP_FAILED) {
        const uint64_t mapped_addr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mapped));
        const uint64_t mapped_end = mapped_addr + static_cast<uint64_t>(aligned_size);
        if (mapped_end <= kMax32BitAddressExclusive) {
            *out_shadow_size = aligned_size;
            return mapped;
        }
        munmap(mapped, aligned_size);
    }
#endif

    for (uintptr_t addr = kShadowSearchStart;
         addr < kShadowSearchEnd &&
             static_cast<uint64_t>(addr) + static_cast<uint64_t>(aligned_size) < kMax32BitAddressExclusive;
         addr += kShadowSearchStep) {
        void* mapped = mmap(reinterpret_cast<void*>(addr), aligned_size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (mapped != MAP_FAILED) {
            *out_shadow_size = aligned_size;
            return mapped;
        }

        if (errno != EEXIST && errno != EINVAL && errno != ENOMEM && errno != EBUSY) {
            break;
        }
    }

    return nullptr;
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

    for (const auto& mapping : stale_mappings) {
        if (mapping.shadow_ptr != nullptr && mapping.shadow_size > 0) {
            munmap(mapping.shadow_ptr, mapping.shadow_size);
        }
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

bool InitializeWrapper() {
    if (getenv("MALI_WRAPPER_DEBUG")) {
        Logger::Instance().SetLevel(LogLevel::DEBUG);
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

    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkGetDeviceProcAddr);
    }

    if (strcmp(pName, "vkCreateDevice") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(internal_vkCreateDevice);
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

    if (is_pointer_32bit_compatible(*ppData)) {
        return;
    }

    if (!should_use_low_address_shadow_map()) {
        return;
    }

    const DeviceMemoryKey key = make_memory_key(device, memory);

    VkDeviceSize resolved_size = 0;
    {
        std::lock_guard<std::mutex> lock(memory_tracking_mutex);
        if (!resolve_map_size_locked(key, offset, size, &resolved_size)) {
            LOG_WARN("Low-address map workaround skipped: unable to resolve mapping size");
            return;
        }
    }

    if (resolved_size == 0 || resolved_size > static_cast<VkDeviceSize>(std::numeric_limits<size_t>::max())) {
        LOG_WARN("Low-address map workaround skipped: mapping size is unsupported");
        return;
    }

    size_t shadow_size = 0;
    void* shadow_ptr = allocate_low_address_shadow(static_cast<size_t>(resolved_size), &shadow_size);
    if (shadow_ptr == nullptr) {
        LOG_WARN("Low-address map workaround failed: unable to allocate shadow mapping");
        return;
    }

    std::memcpy(shadow_ptr, *ppData, static_cast<size_t>(resolved_size));

    ShadowMappingInfo stale_mapping{};
    bool has_stale_mapping = false;
    {
        std::lock_guard<std::mutex> lock(memory_tracking_mutex);
        auto it = shadow_mappings.find(key);
        if (it != shadow_mappings.end()) {
            stale_mapping = it->second;
            has_stale_mapping = true;
            it->second = ShadowMappingInfo{ *ppData, shadow_ptr, shadow_size, offset, resolved_size };
        } else {
            shadow_mappings.emplace(key, ShadowMappingInfo{ *ppData, shadow_ptr, shadow_size, offset, resolved_size });
        }
    }

    if (has_stale_mapping && stale_mapping.shadow_ptr != nullptr && stale_mapping.shadow_size > 0) {
        munmap(stale_mapping.shadow_ptr, stale_mapping.shadow_size);
    }

    *ppData = shadow_ptr;
}

static void sync_shadow_to_real(VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange* pMemoryRanges)
{
    using namespace mali_wrapper;

    if (memoryRangeCount == 0 || pMemoryRanges == nullptr) {
        return;
    }

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
        std::memcpy(real_bytes + byte_offset, shadow_bytes + byte_offset, byte_count);
    }
}

static void sync_real_to_shadow(VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange* pMemoryRanges)
{
    using namespace mali_wrapper;

    if (memoryRangeCount == 0 || pMemoryRanges == nullptr) {
        return;
    }

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
        std::memcpy(shadow_bytes + byte_offset, real_bytes + byte_offset, byte_count);
    }
}

static void sync_all_shadows_for_device(VkDevice device)
{
    using namespace mali_wrapper;

    if (device == VK_NULL_HANDLE) {
        return;
    }

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

        std::memcpy(mapping.real_ptr, mapping.shadow_ptr, static_cast<size_t>(mapping.mapped_size));
    }
}

static void sync_all_shadows()
{
    using namespace mali_wrapper;

    std::lock_guard<std::mutex> lock(memory_tracking_mutex);
    for (auto& entry : shadow_mappings) {
        auto& mapping = entry.second;
        if (mapping.real_ptr == nullptr || mapping.shadow_ptr == nullptr ||
            mapping.mapped_size == 0 ||
            mapping.mapped_size > static_cast<VkDeviceSize>(std::numeric_limits<size_t>::max())) {
            continue;
        }

        std::memcpy(mapping.real_ptr, mapping.shadow_ptr, static_cast<size_t>(mapping.mapped_size));
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
    if (mapping.real_ptr != nullptr && mapping.shadow_ptr != nullptr &&
        mapping.mapped_size > 0 &&
        mapping.mapped_size <= static_cast<VkDeviceSize>(std::numeric_limits<size_t>::max())) {
        std::memcpy(mapping.real_ptr, mapping.shadow_ptr, static_cast<size_t>(mapping.mapped_size));
    }

    if (mapping.shadow_ptr != nullptr && mapping.shadow_size > 0) {
        munmap(mapping.shadow_ptr, mapping.shadow_size);
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

    VkDeviceCreateInfo modified_create_info = *pCreateInfo;
    modified_create_info.enabledExtensionCount = static_cast<uint32_t>(extension_name_count);
    modified_create_info.ppEnabledExtensionNames = extension_name_ptr;

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

    VkResult result = mali_create_device(physicalDevice, pCreateInfo, pAllocator, pDevice);

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
                                                         pCreateInfo->ppEnabledExtensionNames, pCreateInfo->enabledExtensionCount);
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
