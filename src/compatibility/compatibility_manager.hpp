#pragma once

#include "compatibility/pnext_chain.hpp"

#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace mali_wrapper::compatibility {

enum class Profile {
    Native,
    Vkd3d,
};

struct ProfileSelection {
    Profile profile = Profile::Native;
    bool unsafe = false;
};

Profile detect_profile(const VkApplicationInfo* application_info);
const char* profile_name(Profile profile);

void register_instance(VkInstance instance, const VkApplicationInfo* application_info);
void unregister_instance(VkInstance instance);
void associate_physical_device(VkInstance instance, VkPhysicalDevice physical_device);
void unregister_physical_device(VkPhysicalDevice physical_device);
ProfileSelection selection_for(VkPhysicalDevice physical_device);

void overlay_features(ProfileSelection selection, VkPhysicalDeviceFeatures* features);
void overlay_feature_chain(ProfileSelection selection, void* pnext);
void overlay_properties(ProfileSelection selection, VkPhysicalDeviceProperties* properties);
void overlay_property_chain(ProfileSelection selection,
                            VkPhysicalDevice physical_device,
                            void* pnext,
                            PFN_vkGetPhysicalDeviceProperties2 raw_properties2);
void overlay_queue_families(ProfileSelection selection,
                            uint32_t count,
                            VkQueueFamilyProperties* properties);
void overlay_queue_families2(ProfileSelection selection,
                             uint32_t count,
                             VkQueueFamilyProperties2* properties);
void overlay_device_extensions(ProfileSelection selection,
                               std::vector<VkExtensionProperties>* extensions);

class DeviceCreateTransform {
public:
    bool prepare(ProfileSelection selection,
                 const VkDeviceCreateInfo* source,
                 const char* const* extension_names,
                 uint32_t extension_count);

    const VkDeviceCreateInfo* get() const { return &create_info_; }
    const std::string& error() const { return error_; }

private:
    VkDeviceCreateInfo create_info_{};
    VkPhysicalDeviceFeatures enabled_features_{};
    PNextChainCopy pnext_;
    std::vector<const char*> extensions_;
    std::string error_;
};

} // namespace mali_wrapper::compatibility
