#include "wsi_manager.hpp"
#include "mali_wrapper_icd.hpp"
#include "library_loader.hpp"
#include "wsi/surface_api.hpp"
#include "wsi/swapchain_api.hpp"
#include "wsi/wsi_private_data.hpp"
#include <vulkan/vk_layer.h>
#include "wsi/wayland/surface_properties.hpp"
#include "../utils/logging.hpp"
#include <unordered_map>
#include <mutex>
#include <algorithm>
#include <dlfcn.h>
#include <cstdio>
#include <cinttypes>
#include <string>
#include <sstream>
#include <X11/Xlib.h>
#include <xcb/xcb.h>

extern "C" {
    VkResult CreateWaylandSurfaceKHR(VkInstance instance, const VkWaylandSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface);
    VkResult CreateXcbSurfaceKHR(VkInstance instance, const VkXcbSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface);
    VkResult CreateXlibSurfaceKHR(VkInstance instance, const VkXlibSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface);
}

extern "C" {
    VkResult GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface, VkBool32* pSupported);

    void wsi_layer_vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* pAllocator);
    VkResult wsi_layer_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* pSurfaceCapabilities);
    VkResult wsi_layer_vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo, VkSurfaceCapabilities2KHR* pSurfaceCapabilities);
    VkResult wsi_layer_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pSurfaceFormatCount, VkSurfaceFormatKHR* pSurfaceFormats);
    VkResult wsi_layer_vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo, uint32_t* pSurfaceFormatCount, VkSurfaceFormat2KHR* pSurfaceFormats);
    VkResult wsi_layer_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pPresentModeCount, VkPresentModeKHR* pPresentModes);

    VkResult wsi_layer_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain);
    void wsi_layer_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator);
    VkResult wsi_layer_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages);
    VkResult wsi_layer_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex);
    VkResult wsi_layer_vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* pAcquireInfo, uint32_t* pImageIndex);
    VkResult wsi_layer_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
    VkResult wsi_layer_vkGetSwapchainStatusKHR(VkDevice device, VkSwapchainKHR swapchain);
}

extern "C" {
    VkBool32 GetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, struct wl_display* display);
    VkBool32 GetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
                                                        xcb_connection_t* connection, xcb_visualid_t visual_id);
    VkBool32 GetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
                                                         Display* dpy, VisualID visualID);
}

namespace mali_wrapper {

namespace {
VkResult VKAPI_CALL NoOpSetInstanceLoaderData(VkInstance, void *)
{
   return VK_SUCCESS;
}

VkResult VKAPI_CALL NoOpSetDeviceLoaderData(VkDevice device, void *object)
{
   if (object != nullptr)
   {
      register_queue_key_mapping(device, reinterpret_cast<VkQueue>(object));
   }
   return VK_SUCCESS;
}
} // namespace

bool is_dummy_surface(VkSurfaceKHR surface) {
    uint64_t surface_handle = reinterpret_cast<uint64_t>(surface);
    return (surface_handle == 0x12345678 || surface_handle == 0x12345679 ||
            surface_handle == 0x1234567A || surface_handle == 0x1234567B);
}

static WSIManager *g_wsi_manager = nullptr;

static std::mutex &GetGlobalManagerMutex()
{
    static auto *mutex = new std::mutex();
    return *mutex;
}

WSIManager& GetWSIManager() {
    std::lock_guard<std::mutex> lock(GetGlobalManagerMutex());
    if (g_wsi_manager == nullptr) {
        g_wsi_manager = new WSIManager();
    }
    return *g_wsi_manager;
}

struct WSIManager::Impl {
    std::mutex manager_mutex;
    std::unordered_map<VkInstance, std::unique_ptr<instance_private_data>> instances;
    std::unordered_map<VkDevice, std::unique_ptr<device_private_data>> devices;
    bool initialized = false;
    bool cleaned_up = false;

    PFN_vkGetPhysicalDeviceFeatures2KHR mali_GetPhysicalDeviceFeatures2KHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR mali_GetPhysicalDeviceSurfaceSupportKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR mali_GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR mali_GetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR mali_GetPhysicalDeviceSurfacePresentModesKHR = nullptr;
};

WSIManager::WSIManager() : pImpl(std::make_unique<Impl>()) {
}

WSIManager::~WSIManager() {
    cleanup();
}

VkResult WSIManager::initialize(VkInstance instance, VkPhysicalDevice physicalDevice) {
    std::lock_guard<std::mutex> lock(pImpl->manager_mutex);

    LOG_INFO("WSIManager: Initializing for instance");

    if (pImpl->instances.find(instance) != pImpl->instances.end()) {
        LOG_WARN("Instance already initialized");
        return VK_SUCCESS;
    }

    util::allocator wsi_allocator(VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE, nullptr);

    auto dispatch_table = instance_dispatch_table::create(wsi_allocator);
    if (!dispatch_table.has_value()) {
        LOG_ERROR("Failed to create instance dispatch table");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    auto& loader = LibraryLoader::Instance();

    PFN_vkGetInstanceProcAddr mali_get_instance_proc_addr = loader.GetMaliGetInstanceProcAddr();

    if (!mali_get_instance_proc_addr) {
        LOG_ERROR("Failed to get Mali's vkGetInstanceProcAddr - Mali driver may not be loaded properly");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    auto result = dispatch_table->populate(instance, mali_get_instance_proc_addr);
    if (result != VK_SUCCESS) {
        LOG_ERROR("Failed to populate instance dispatch table with Mali functions");
        return result;
    }

    util::wsi_platform_set platforms;

#if BUILD_WSI_X11
    platforms.add(VK_ICD_WSI_PLATFORM_XCB);
    platforms.add(VK_ICD_WSI_PLATFORM_XLIB);
#endif
#if BUILD_WSI_WAYLAND
    platforms.add(VK_ICD_WSI_PLATFORM_WAYLAND);
#endif
#if BUILD_WSI_HEADLESS
    platforms.add(VK_ICD_WSI_PLATFORM_HEADLESS);
#endif

    result = instance_private_data::associate(instance, std::move(*dispatch_table), NoOpSetInstanceLoaderData,
                                              platforms, VK_API_VERSION_1_0, wsi_allocator);
    if (result != VK_SUCCESS) {
        LOG_ERROR("Failed to associate instance private data");
        return result;
    }

    auto& instance_data = instance_private_data::get(instance);

    instance_data.set_mali_functions(
        reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2KHR>(loader.GetMaliProcAddr("vkGetPhysicalDeviceFeatures2KHR")),
        reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(loader.GetMaliProcAddr("vkGetPhysicalDeviceSurfaceSupportKHR")),
        reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(loader.GetMaliProcAddr("vkGetPhysicalDeviceSurfaceCapabilitiesKHR")),
        reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(loader.GetMaliProcAddr("vkGetPhysicalDeviceSurfaceFormatsKHR")),
        reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(loader.GetMaliProcAddr("vkGetPhysicalDeviceSurfacePresentModesKHR"))
    );

    pImpl->instances.emplace(instance, nullptr);
    pImpl->cleaned_up = false;
    pImpl->initialized = true;
    LOG_INFO("WSIManager: Successfully initialized instance");
    return VK_SUCCESS;
}

VkResult WSIManager::init_device(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice mali_device,
                                 const char *const *enabled_extensions, size_t enabled_extension_count) {
    std::lock_guard<std::mutex> lock(pImpl->manager_mutex);

    LOG_DEBUG("WSIManager::init_device called, device=0x" + std::to_string(reinterpret_cast<uintptr_t>(mali_device)) +
              " instance=0x" + std::to_string(reinterpret_cast<uintptr_t>(instance)));

    if (pImpl->devices.find(mali_device) != pImpl->devices.end()) {
        LOG_WARN("Device already initialized");
        return VK_SUCCESS;
    }

    auto& instance_data = instance_private_data::get(instance);

    util::allocator wsi_allocator(VK_SYSTEM_ALLOCATION_SCOPE_DEVICE, nullptr);

    auto device_dispatch_table = device_dispatch_table::create(wsi_allocator);
    if (!device_dispatch_table.has_value()) {
        LOG_ERROR("Failed to create device dispatch table");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    auto& loader = LibraryLoader::Instance();
    PFN_vkGetInstanceProcAddr mali_get_instance_proc_addr = loader.GetMaliGetInstanceProcAddr();

    if (!mali_get_instance_proc_addr) {
        LOG_ERROR("Mali instance proc addr is NULL");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetDeviceProcAddr mali_get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        mali_get_instance_proc_addr(instance, "vkGetDeviceProcAddr"));

    if (!mali_get_device_proc_addr) {
        LOG_ERROR("Mali device proc addr is NULL");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    auto result = device_dispatch_table->populate(mali_device, mali_get_device_proc_addr);
    if (result != VK_SUCCESS) {
        LOG_ERROR("Failed to populate device dispatch table");
        return result;
    }

    result = device_private_data::associate(mali_device, instance_data, physicalDevice,
                                          std::move(*device_dispatch_table), NoOpSetDeviceLoaderData, wsi_allocator);
    if (result != VK_SUCCESS) {
        LOG_ERROR("Failed to associate device private data");
        return result;
    }

    auto& device_data = device_private_data::get(mali_device);

    if (enabled_extensions != nullptr && enabled_extension_count > 0)
    {
        auto ext_result = device_data.set_device_enabled_extensions(enabled_extensions, enabled_extension_count);
        if (ext_result != VK_SUCCESS)
        {
            LOG_WARN("Failed to record enabled device extensions, error: " + std::to_string(ext_result));
        }
    }

    const bool has_compression_control =
        device_data.is_device_extension_enabled(VK_EXT_IMAGE_COMPRESSION_CONTROL_EXTENSION_NAME);
    device_data.set_swapchain_compression_control_enabled(has_compression_control);

#ifdef VK_EXT_FRAME_BOUNDARY_EXTENSION_NAME
    const bool has_frame_boundary =
        device_data.is_device_extension_enabled(VK_EXT_FRAME_BOUNDARY_EXTENSION_NAME);
#else
    const bool has_frame_boundary = false;
#endif
    device_data.set_layer_frame_boundary_handling_enabled(has_frame_boundary);

#ifdef VK_KHR_PRESENT_ID_EXTENSION_NAME
    const bool has_present_id = device_data.is_device_extension_enabled(VK_KHR_PRESENT_ID_EXTENSION_NAME);
#else
    const bool has_present_id = false;
#endif
    device_data.set_present_id_feature_enabled(has_present_id);

#ifdef VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME
    const bool has_swapchain_maintenance1 =
        device_data.is_device_extension_enabled(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
#else
    const bool has_swapchain_maintenance1 = false;
#endif
    device_data.set_swapchain_maintenance1_enabled(has_swapchain_maintenance1);

    device_data.set_mali_functions(
        reinterpret_cast<PFN_vkCreateSwapchainKHR>(loader.GetMaliProcAddr("vkCreateSwapchainKHR")),
        reinterpret_cast<PFN_vkDestroySwapchainKHR>(loader.GetMaliProcAddr("vkDestroySwapchainKHR")),
        reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(loader.GetMaliProcAddr("vkGetSwapchainImagesKHR")),
        reinterpret_cast<PFN_vkAcquireNextImageKHR>(loader.GetMaliProcAddr("vkAcquireNextImageKHR")),
        reinterpret_cast<PFN_vkQueuePresentKHR>(loader.GetMaliProcAddr("vkQueuePresentKHR"))
    );

    pImpl->devices.emplace(mali_device, nullptr);

    LOG_INFO("WSIManager: Successfully initialized device");
    return VK_SUCCESS;
}

void WSIManager::cleanup() {
    std::lock_guard<std::mutex> lock(pImpl->manager_mutex);

    if (pImpl->cleaned_up) {
        return; // Already cleaned up
    }

    for (const auto &entry : pImpl->devices) {
        device_private_data::disassociate(entry.first);
    }
    pImpl->devices.clear();

    for (const auto &entry : pImpl->instances) {
        instance_private_data::disassociate(entry.first);
    }
    pImpl->instances.clear();

    pImpl->initialized = false;
    pImpl->cleaned_up = true;
}

void WSIManager::release_device(VkDevice device) {

    {
        std::lock_guard<std::mutex> lock(pImpl->manager_mutex);
        pImpl->devices.erase(device);
    }

    auto *device_ptr = device_private_data::try_get(device);
    if (device_ptr == nullptr)
    {
        mali_wrapper::Logger::Instance().Debug([device]{
            std::ostringstream oss;
            oss << "WSIManager: release_device no private data for " << static_cast<void*>(device);
            return oss.str();
        }());
        return;
    }

    device_private_data::disassociate(device);
}

void WSIManager::release_instance(VkInstance instance) {

    {
        std::lock_guard<std::mutex> lock(pImpl->manager_mutex);
        pImpl->instances.erase(instance);
    }

    if (instance_private_data::try_get(instance) == nullptr)
    {
        mali_wrapper::Logger::Instance().Debug([instance]{
            std::ostringstream oss;
            oss << "WSIManager: release_instance no private data for " << static_cast<void*>(instance);
            return oss.str();
        }());
        return;
    }

    instance_private_data::disassociate(instance);
}

VkResult WSIManager::create_surface_xcb(VkInstance instance, const VkXcbSurfaceCreateInfoKHR* pCreateInfo,
                                       const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {

    mali_wrapper::add_instance_reference(instance);

    VkResult result = CreateXcbSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);

    if (result != VK_SUCCESS) {
        mali_wrapper::remove_instance_reference(instance);
    }

    return result;
}

VkResult WSIManager::create_surface_xlib(VkInstance instance, const VkXlibSurfaceCreateInfoKHR* pCreateInfo,
                                        const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {

    mali_wrapper::add_instance_reference(instance);

    VkResult result = CreateXlibSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);

    if (result != VK_SUCCESS) {
        mali_wrapper::remove_instance_reference(instance);
    }

    return result;
}

VkResult WSIManager::create_surface_wayland(VkInstance instance, const VkWaylandSurfaceCreateInfoKHR* pCreateInfo,
                                           const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {

    mali_wrapper::add_instance_reference(instance);

    VkResult result = CreateWaylandSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);

    if (result != VK_SUCCESS) {
        mali_wrapper::remove_instance_reference(instance);
    }

    return result;
}

VkResult WSIManager::create_surface_headless(VkInstance instance, const VkHeadlessSurfaceCreateInfoEXT* pCreateInfo,
                                            const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
    LOG_WARN("Surface creation not yet implemented - returning dummy surface");
    *pSurface = reinterpret_cast<VkSurfaceKHR>(static_cast<uint64_t>(0x1234567B)); // Dummy surface handle
    return VK_SUCCESS;
}

VkResult WSIManager::destroy_surface(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* pAllocator) {

    if (is_dummy_surface(surface)) {
        LOG_WARN("Dummy surface destruction - no action needed");
        return VK_SUCCESS;
    }

    wsi_layer_vkDestroySurfaceKHR(instance, surface, pAllocator);

    mali_wrapper::remove_instance_reference(instance);

    return VK_SUCCESS;
}

VkResult WSIManager::get_surface_support(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
                                        VkSurfaceKHR surface, VkBool32* pSupported) {

    if (is_dummy_surface(surface)) {
        LOG_WARN("Dummy surface detected, returning supported = VK_TRUE");
        *pSupported = VK_TRUE;
        return VK_SUCCESS;
    }

    return GetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamilyIndex, surface, pSupported);
}

VkResult WSIManager::get_surface_capabilities(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                             VkSurfaceCapabilitiesKHR* pSurfaceCapabilities) {

    if (is_dummy_surface(surface)) {
        LOG_WARN("Dummy surface detected, returning default capabilities");

        pSurfaceCapabilities->minImageCount = 2;
        pSurfaceCapabilities->maxImageCount = 8;
        pSurfaceCapabilities->currentExtent = {1920, 1080};
        pSurfaceCapabilities->minImageExtent = {1, 1};
        pSurfaceCapabilities->maxImageExtent = {4096, 4096};
        pSurfaceCapabilities->maxImageArrayLayers = 1;
        pSurfaceCapabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        pSurfaceCapabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        pSurfaceCapabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        pSurfaceCapabilities->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        return VK_SUCCESS;
    }

    return wsi_layer_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, pSurfaceCapabilities);
}

VkResult WSIManager::get_surface_capabilities2(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
                                              VkSurfaceCapabilities2KHR* pSurfaceCapabilities) {

    if (pSurfaceInfo && is_dummy_surface(pSurfaceInfo->surface)) {
        LOG_WARN("Dummy surface detected in surface info, returning default capabilities");

        pSurfaceCapabilities->sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
        pSurfaceCapabilities->pNext = nullptr;

        pSurfaceCapabilities->surfaceCapabilities.minImageCount = 2;
        pSurfaceCapabilities->surfaceCapabilities.maxImageCount = 8;
        pSurfaceCapabilities->surfaceCapabilities.currentExtent = {1920, 1080};
        pSurfaceCapabilities->surfaceCapabilities.minImageExtent = {1, 1};
        pSurfaceCapabilities->surfaceCapabilities.maxImageExtent = {4096, 4096};
        pSurfaceCapabilities->surfaceCapabilities.maxImageArrayLayers = 1;
        pSurfaceCapabilities->surfaceCapabilities.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        pSurfaceCapabilities->surfaceCapabilities.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        pSurfaceCapabilities->surfaceCapabilities.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        pSurfaceCapabilities->surfaceCapabilities.supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        return VK_SUCCESS;
    }

    return wsi_layer_vkGetPhysicalDeviceSurfaceCapabilities2KHR(physicalDevice, pSurfaceInfo, pSurfaceCapabilities);
}

VkResult WSIManager::get_surface_formats(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                        uint32_t* pSurfaceFormatCount, VkSurfaceFormatKHR* pSurfaceFormats) {

    if (is_dummy_surface(surface)) {
        LOG_WARN("Dummy surface detected, returning default formats");

        const VkSurfaceFormatKHR default_formats[] = {
            {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
            {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
        };
        const uint32_t format_count = sizeof(default_formats) / sizeof(default_formats[0]);

        if (pSurfaceFormats == nullptr) {
            *pSurfaceFormatCount = format_count;
            return VK_SUCCESS;
        }

        uint32_t copy_count = std::min(*pSurfaceFormatCount, format_count);
        for (uint32_t i = 0; i < copy_count; i++) {
            pSurfaceFormats[i] = default_formats[i];
        }
        *pSurfaceFormatCount = copy_count;

        return (copy_count < format_count) ? VK_INCOMPLETE : VK_SUCCESS;
    }

    return wsi_layer_vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);
}

VkResult WSIManager::get_surface_formats2(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
                                         uint32_t* pSurfaceFormatCount, VkSurfaceFormat2KHR* pSurfaceFormats) {

    if (pSurfaceInfo && is_dummy_surface(pSurfaceInfo->surface)) {
        LOG_WARN("Dummy surface detected in surface info, returning default formats");

        const VkSurfaceFormat2KHR default_formats[] = {
            {VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR, nullptr, {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}},
            {VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR, nullptr, {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}}
        };
        const uint32_t format_count = sizeof(default_formats) / sizeof(default_formats[0]);

        if (pSurfaceFormats == nullptr) {
            *pSurfaceFormatCount = format_count;
            return VK_SUCCESS;
        }

        uint32_t copy_count = std::min(*pSurfaceFormatCount, format_count);
        for (uint32_t i = 0; i < copy_count; i++) {
            pSurfaceFormats[i] = default_formats[i];
        }
        *pSurfaceFormatCount = copy_count;

        return (copy_count < format_count) ? VK_INCOMPLETE : VK_SUCCESS;
    }

    return wsi_layer_vkGetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, pSurfaceInfo, pSurfaceFormatCount, pSurfaceFormats);
}

VkResult WSIManager::get_surface_present_modes(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                              uint32_t* pPresentModeCount, VkPresentModeKHR* pPresentModes) {

    if (is_dummy_surface(surface)) {
        LOG_WARN("Dummy surface detected, returning default present modes");

        const VkPresentModeKHR default_modes[] = {
            VK_PRESENT_MODE_FIFO_KHR,
            VK_PRESENT_MODE_MAILBOX_KHR
        };
        const uint32_t mode_count = sizeof(default_modes) / sizeof(default_modes[0]);

        if (pPresentModes == nullptr) {
            *pPresentModeCount = mode_count;
            return VK_SUCCESS;
        }

        uint32_t copy_count = std::min(*pPresentModeCount, mode_count);
        for (uint32_t i = 0; i < copy_count; i++) {
            pPresentModes[i] = default_modes[i];
        }
        *pPresentModeCount = copy_count;

        return (copy_count < mode_count) ? VK_INCOMPLETE : VK_SUCCESS;
    }

    return wsi_layer_vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, pPresentModeCount, pPresentModes);
}

VkBool32 WSIManager::get_wayland_presentation_support(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
                                                     struct wl_display* display) {
    return GetPhysicalDeviceWaylandPresentationSupportKHR(physicalDevice, queueFamilyIndex, display);
}

VkResult WSIManager::create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                                     const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {

    VkResult result = wsi_layer_vkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    return result;
}

VkResult WSIManager::destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator) {
    wsi_layer_vkDestroySwapchainKHR(device, swapchain, pAllocator);
    return VK_SUCCESS;
}

VkResult WSIManager::get_swapchain_images(VkDevice device, VkSwapchainKHR swapchain,
                                         uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages) {
    return wsi_layer_vkGetSwapchainImagesKHR(device, swapchain, pSwapchainImageCount, pSwapchainImages);
}

VkResult WSIManager::acquire_next_image(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                       VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex) {
    return wsi_layer_vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

VkResult WSIManager::acquire_next_image2(VkDevice device, const VkAcquireNextImageInfoKHR* pAcquireInfo, uint32_t* pImageIndex) {
    return wsi_layer_vkAcquireNextImage2KHR(device, pAcquireInfo, pImageIndex);
}

VkResult WSIManager::queue_present(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    return wsi_layer_vkQueuePresentKHR(queue, pPresentInfo);
}

VkResult WSIManager::get_swapchain_status(VkDevice device, VkSwapchainKHR swapchain) {
    return wsi_layer_vkGetSwapchainStatusKHR(device, swapchain);
}

VkResult WSIManager::get_device_group_present_capabilities(VkDevice device, VkDeviceGroupPresentCapabilitiesKHR* pDeviceGroupPresentCapabilities) {
    return wsi_GetDeviceGroupPresentCapabilitiesKHR(device, pDeviceGroupPresentCapabilities);
}

VkResult WSIManager::get_device_group_surface_present_modes(VkDevice device, VkSurfaceKHR surface, VkDeviceGroupPresentModeFlagsKHR* pModes) {
    return wsi_GetDeviceGroupSurfacePresentModesKHR(device, surface, pModes);
}

VkResult WSIManager::get_physical_device_present_rectangles(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                                           uint32_t* pRectCount, VkRect2D* pRects) {
    return wsi_GetPhysicalDevicePresentRectanglesKHR(physicalDevice, surface, pRectCount, pRects);
}

bool WSIManager::is_wsi_function(const char* function_name) {
    if (!function_name) return false;

    // Surface functions
    if (strcmp(function_name, "vkCreateXlibSurfaceKHR") == 0) return true;
    if (strcmp(function_name, "vkCreateXcbSurfaceKHR") == 0) return true;
    if (strcmp(function_name, "vkCreateWaylandSurfaceKHR") == 0) return true;
    if (strcmp(function_name, "vkCreateDisplaySurfaceKHR") == 0) return true;
    if (strcmp(function_name, "vkCreateHeadlessSurfaceEXT") == 0) return true;
    if (strcmp(function_name, "vkDestroySurfaceKHR") == 0) return true;
    if (strcmp(function_name, "vkGetPhysicalDeviceSurfaceSupportKHR") == 0) return true;
    if (strcmp(function_name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0) return true;
    if (strcmp(function_name, "vkGetPhysicalDeviceSurfaceCapabilities2KHR") == 0) return true;
    if (strcmp(function_name, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0) return true;
    if (strcmp(function_name, "vkGetPhysicalDeviceSurfaceFormats2KHR") == 0) return true;
    if (strcmp(function_name, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0) return true;
    if (strcmp(function_name, "vkGetPhysicalDeviceWaylandPresentationSupportKHR") == 0) return true;
    if (strcmp(function_name, "vkGetPhysicalDeviceXcbPresentationSupportKHR") == 0) return true;
    if (strcmp(function_name, "vkGetPhysicalDeviceXlibPresentationSupportKHR") == 0) return true;

    // Swapchain functions
    if (strcmp(function_name, "vkCreateSwapchainKHR") == 0) return true;
    if (strcmp(function_name, "vkDestroySwapchainKHR") == 0) return true;
    if (strcmp(function_name, "vkGetSwapchainImagesKHR") == 0) return true;
    if (strcmp(function_name, "vkAcquireNextImageKHR") == 0) return true;
    if (strcmp(function_name, "vkAcquireNextImage2KHR") == 0) return true;
    if (strcmp(function_name, "vkQueuePresentKHR") == 0) return true;
    if (strcmp(function_name, "vkGetSwapchainStatusKHR") == 0) return true;

    // Device group functions
    if (strcmp(function_name, "vkGetDeviceGroupPresentCapabilitiesKHR") == 0) return true;
    if (strcmp(function_name, "vkGetDeviceGroupSurfacePresentModesKHR") == 0) return true;
    if (strcmp(function_name, "vkGetPhysicalDevicePresentRectanglesKHR") == 0) return true;

    return false;
}

static VKAPI_ATTR VkResult VKAPI_CALL static_vkCreateXcbSurfaceKHR(VkInstance instance, const VkXcbSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
    return GetWSIManager().create_surface_xcb(instance, pCreateInfo, pAllocator, pSurface);
}

static VKAPI_ATTR VkResult VKAPI_CALL static_vkCreateXlibSurfaceKHR(VkInstance instance, const VkXlibSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
    return GetWSIManager().create_surface_xlib(instance, pCreateInfo, pAllocator, pSurface);
}

static VKAPI_ATTR VkResult VKAPI_CALL static_vkCreateWaylandSurfaceKHR(VkInstance instance, const VkWaylandSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
    return GetWSIManager().create_surface_wayland(instance, pCreateInfo, pAllocator, pSurface);
}

static VKAPI_ATTR VkResult VKAPI_CALL static_vkCreateHeadlessSurfaceEXT(VkInstance instance, const VkHeadlessSurfaceCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
    return GetWSIManager().create_surface_headless(instance, pCreateInfo, pAllocator, pSurface);
}

static VKAPI_ATTR void VKAPI_CALL static_vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* pAllocator) {
    GetWSIManager().destroy_surface(instance, surface, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL static_vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface, VkBool32* pSupported) {
    return GetWSIManager().get_surface_support(physicalDevice, queueFamilyIndex, surface, pSupported);
}

static VKAPI_ATTR VkResult VKAPI_CALL static_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* pSurfaceCapabilities) {
    return GetWSIManager().get_surface_capabilities(physicalDevice, surface, pSurfaceCapabilities);
}

static VKAPI_ATTR VkResult VKAPI_CALL static_vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo, VkSurfaceCapabilities2KHR* pSurfaceCapabilities) {
    return GetWSIManager().get_surface_capabilities2(physicalDevice, pSurfaceInfo, pSurfaceCapabilities);
}

static VKAPI_ATTR VkResult VKAPI_CALL static_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pSurfaceFormatCount, VkSurfaceFormatKHR* pSurfaceFormats) {
    return GetWSIManager().get_surface_formats(physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);
}

static VKAPI_ATTR VkResult VKAPI_CALL static_vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo, uint32_t* pSurfaceFormatCount, VkSurfaceFormat2KHR* pSurfaceFormats) {
    return GetWSIManager().get_surface_formats2(physicalDevice, pSurfaceInfo, pSurfaceFormatCount, pSurfaceFormats);
}

static VKAPI_ATTR VkResult VKAPI_CALL static_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pPresentModeCount, VkPresentModeKHR* pPresentModes) {
    return GetWSIManager().get_surface_present_modes(physicalDevice, surface, pPresentModeCount, pPresentModes);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL static_vkGetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, struct wl_display* display) {
    return GetWSIManager().get_wayland_presentation_support(physicalDevice, queueFamilyIndex, display);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL static_vkGetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, xcb_connection_t* connection, xcb_visualid_t visual_id) {
    return GetPhysicalDeviceXcbPresentationSupportKHR(physicalDevice, queueFamilyIndex, connection, visual_id);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL static_vkGetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, Display* dpy, VisualID visualID) {
    return GetPhysicalDeviceXlibPresentationSupportKHR(physicalDevice, queueFamilyIndex, dpy, visualID);
}

static VKAPI_ATTR VkResult VKAPI_CALL static_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
    return GetWSIManager().create_swapchain(device, pCreateInfo, pAllocator, pSwapchain);
}

static VKAPI_ATTR void VKAPI_CALL static_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator) {
    GetWSIManager().destroy_swapchain(device, swapchain, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL static_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages) {
    return GetWSIManager().get_swapchain_images(device, swapchain, pSwapchainImageCount, pSwapchainImages);
}

static VKAPI_ATTR VkResult VKAPI_CALL static_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex) {
    return GetWSIManager().acquire_next_image(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

static VKAPI_ATTR VkResult VKAPI_CALL static_vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* pAcquireInfo, uint32_t* pImageIndex) {
    return GetWSIManager().acquire_next_image2(device, pAcquireInfo, pImageIndex);
}

static VKAPI_ATTR VkResult VKAPI_CALL static_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    return GetWSIManager().queue_present(queue, pPresentInfo);
}

static VKAPI_ATTR VkResult VKAPI_CALL static_vkGetSwapchainStatusKHR(VkDevice device, VkSwapchainKHR swapchain) {
    return GetWSIManager().get_swapchain_status(device, swapchain);
}

PFN_vkVoidFunction WSIManager::get_function_pointer(const char* function_name) {
    if (!function_name || !is_wsi_function(function_name)) {
        return nullptr;
    }

    // Surface creation functions
    if (strcmp(function_name, "vkCreateXcbSurfaceKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkCreateXcbSurfaceKHR);
    }
    if (strcmp(function_name, "vkCreateXlibSurfaceKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkCreateXlibSurfaceKHR);
    }
    if (strcmp(function_name, "vkCreateWaylandSurfaceKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkCreateWaylandSurfaceKHR);
    }
    if (strcmp(function_name, "vkCreateHeadlessSurfaceEXT") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkCreateHeadlessSurfaceEXT);
    }
    if (strcmp(function_name, "vkDestroySurfaceKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkDestroySurfaceKHR);
    }

    // Surface property functions
    if (strcmp(function_name, "vkGetPhysicalDeviceSurfaceSupportKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkGetPhysicalDeviceSurfaceSupportKHR);
    }
    if (strcmp(function_name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    }
    if (strcmp(function_name, "vkGetPhysicalDeviceSurfaceCapabilities2KHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkGetPhysicalDeviceSurfaceCapabilities2KHR);
    }
    if (strcmp(function_name, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkGetPhysicalDeviceSurfaceFormatsKHR);
    }
    if (strcmp(function_name, "vkGetPhysicalDeviceSurfaceFormats2KHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkGetPhysicalDeviceSurfaceFormats2KHR);
    }
    if (strcmp(function_name, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkGetPhysicalDeviceSurfacePresentModesKHR);
    }
    if (strcmp(function_name, "vkGetPhysicalDeviceWaylandPresentationSupportKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkGetPhysicalDeviceWaylandPresentationSupportKHR);
    }
    if (strcmp(function_name, "vkGetPhysicalDeviceXcbPresentationSupportKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkGetPhysicalDeviceXcbPresentationSupportKHR);
    }
    if (strcmp(function_name, "vkGetPhysicalDeviceXlibPresentationSupportKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkGetPhysicalDeviceXlibPresentationSupportKHR);
    }

    // Swapchain functions
    if (strcmp(function_name, "vkCreateSwapchainKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkCreateSwapchainKHR);
    }
    if (strcmp(function_name, "vkDestroySwapchainKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkDestroySwapchainKHR);
    }
    if (strcmp(function_name, "vkGetSwapchainImagesKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkGetSwapchainImagesKHR);
    }
    if (strcmp(function_name, "vkAcquireNextImageKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkAcquireNextImageKHR);
    }
    if (strcmp(function_name, "vkAcquireNextImage2KHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkAcquireNextImage2KHR);
    }
    if (strcmp(function_name, "vkQueuePresentKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkQueuePresentKHR);
    }
    if (strcmp(function_name, "vkGetSwapchainStatusKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(static_vkGetSwapchainStatusKHR);
    }

    return nullptr;
}


instance_private_data* WSIManager::lookup_instance(VkInstance instance) {
    std::lock_guard<std::mutex> lock(pImpl->manager_mutex);
    auto it = pImpl->instances.find(instance);
    return (it != pImpl->instances.end()) ? it->second.get() : nullptr;
}

instance_private_data* WSIManager::lookup_first_instance() {
    std::lock_guard<std::mutex> lock(pImpl->manager_mutex);
    return pImpl->instances.empty() ? nullptr : pImpl->instances.begin()->second.get();
}

device_private_data* WSIManager::lookup_device(VkDevice device) {
    std::lock_guard<std::mutex> lock(pImpl->manager_mutex);
    auto it = pImpl->devices.find(device);
    return (it != pImpl->devices.end()) ? it->second.get() : nullptr;
}

device_private_data* WSIManager::lookup_first_device() {
    std::lock_guard<std::mutex> lock(pImpl->manager_mutex);
    return pImpl->devices.empty() ? nullptr : pImpl->devices.begin()->second.get();
}

} // namespace mali_wrapper
