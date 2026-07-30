#include "compatibility/compatibility_manager.hpp"

#include "utils/logging.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace mali_wrapper::compatibility {
namespace {

struct InstanceProfile {
    ProfileSelection selection;
    std::string application_name;
    std::string engine_name;
};

std::mutex profile_mutex;
std::unordered_map<VkInstance, InstanceProfile> instance_profiles;
std::unordered_map<VkPhysicalDevice, VkInstance> physical_device_instances;

bool equals_ascii_case_insensitive(const char* left, const char* right)
{
    if (left == nullptr || right == nullptr) {
        return false;
    }
    while (*left != '\0' && *right != '\0') {
        if (std::tolower(static_cast<unsigned char>(*left)) !=
            std::tolower(static_cast<unsigned char>(*right))) {
            return false;
        }
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

bool bool_environment(const char* name, bool fallback)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    return value[0] != '0' && value[0] != 'n' && value[0] != 'N' &&
           value[0] != 'f' && value[0] != 'F';
}

ProfileSelection select_profile(const VkApplicationInfo* application_info)
{
    ProfileSelection selection;
    const char* override_value = std::getenv("MALI_WRAPPER_COMPAT_PROFILE");

    if (override_value != nullptr && equals_ascii_case_insensitive(override_value, "off")) {
        return selection;
    }
    if (override_value != nullptr && equals_ascii_case_insensitive(override_value, "vkd3d")) {
        selection.profile = Profile::Vkd3d;
    } else {
        selection.profile = detect_profile(application_info);
    }

    selection.unsafe = selection.profile == Profile::Vkd3d &&
                       bool_environment("MALI_WRAPPER_UNSAFE_SPOOF", false);
    return selection;
}

template <typename T>
T* find_output_structure(void* pnext, VkStructureType type)
{
    auto* current = static_cast<VkBaseOutStructure*>(pnext);
    while (current != nullptr) {
        if (current->sType == type) {
            return reinterpret_cast<T*>(current);
        }
        current = current->pNext;
    }
    return nullptr;
}

template <typename T>
T* find_input_structure(void* pnext, VkStructureType type)
{
    auto* current = static_cast<VkBaseOutStructure*>(pnext);
    while (current != nullptr) {
        if (current->sType == type) {
            return reinterpret_cast<T*>(current);
        }
        current = current->pNext;
    }
    return nullptr;
}

bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name)
{
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

void sanitize_core_features(ProfileSelection selection, VkPhysicalDeviceFeatures* features)
{
    if (features == nullptr) {
        return;
    }

    features->pipelineStatisticsQuery = VK_FALSE;
    if (!selection.unsafe) {
        return;
    }

    features->vertexPipelineStoresAndAtomics = VK_FALSE;
    features->sparseBinding = VK_FALSE;
    features->sparseResidencyBuffer = VK_FALSE;
    features->sparseResidencyImage2D = VK_FALSE;
    features->sparseResidencyAliased = VK_FALSE;
    features->shaderResourceResidency = VK_FALSE;
    features->shaderResourceMinLod = VK_FALSE;
}

} // namespace

Profile detect_profile(const VkApplicationInfo* application_info)
{
    if (application_info == nullptr || application_info->pEngineName == nullptr) {
        return Profile::Native;
    }
    if (equals_ascii_case_insensitive(application_info->pEngineName, "vkd3d")) {
        return Profile::Vkd3d;
    }
    // The unified DXVK build consumes the real Mali feature set and implements
    // its own shader/pipeline fallbacks. It intentionally uses Native here.
    return Profile::Native;
}

const char* profile_name(Profile profile)
{
    return profile == Profile::Vkd3d ? "vkd3d" : "native";
}

void register_instance(VkInstance instance, const VkApplicationInfo* application_info)
{
    InstanceProfile state;
    state.selection = select_profile(application_info);
    if (application_info != nullptr) {
        if (application_info->pApplicationName != nullptr) {
            state.application_name = application_info->pApplicationName;
        }
        if (application_info->pEngineName != nullptr) {
            state.engine_name = application_info->pEngineName;
        }
    }

    {
        std::lock_guard<std::mutex> lock(profile_mutex);
        instance_profiles[instance] = state;
    }

    std::string summary = "Compatibility profile: profile=";
    summary += profile_name(state.selection.profile);
    summary += ", unsafe=";
    summary += state.selection.unsafe ? "enabled" : "disabled";
    summary += ", engine=";
    summary += state.engine_name.empty() ? "<unspecified>" : state.engine_name;
    if (state.selection.profile == Profile::Vkd3d) {
        summary += state.selection.unsafe
            ? ", bounded=pipeline-statistics, unsafe=zero-divisor/robustness2/texel-alignment/sparse"
            : ", bounded=pipeline-statistics, unsupported-gaps=not-advertised";
    }
    LOG_INFO(summary);
}

void unregister_instance(VkInstance instance)
{
    std::lock_guard<std::mutex> lock(profile_mutex);
    instance_profiles.erase(instance);
    for (auto it = physical_device_instances.begin(); it != physical_device_instances.end();) {
        if (it->second == instance) {
            it = physical_device_instances.erase(it);
        } else {
            ++it;
        }
    }
}

void associate_physical_device(VkInstance instance, VkPhysicalDevice physical_device)
{
    if (instance == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lock(profile_mutex);
    physical_device_instances[physical_device] = instance;
}

void unregister_physical_device(VkPhysicalDevice physical_device)
{
    std::lock_guard<std::mutex> lock(profile_mutex);
    physical_device_instances.erase(physical_device);
}

ProfileSelection selection_for(VkPhysicalDevice physical_device)
{
    std::lock_guard<std::mutex> lock(profile_mutex);
    auto physical_it = physical_device_instances.find(physical_device);
    if (physical_it != physical_device_instances.end()) {
        auto instance_it = instance_profiles.find(physical_it->second);
        if (instance_it != instance_profiles.end()) {
            return instance_it->second.selection;
        }
    }
    // A few loaders query a physical device before the application's explicit
    // enumeration. Falling back is unambiguous only with one live instance.
    if (instance_profiles.size() == 1) {
        return instance_profiles.begin()->second.selection;
    }
    return {};
}

void overlay_features(ProfileSelection selection, VkPhysicalDeviceFeatures* features)
{
    if (features == nullptr || selection.profile != Profile::Vkd3d) {
        return;
    }

    // This feature is completely intercepted by the bounded query emulator.
    features->pipelineStatisticsQuery = VK_TRUE;

    if (!selection.unsafe) {
        return;
    }

    features->vertexPipelineStoresAndAtomics = VK_TRUE;
    features->sparseBinding = VK_TRUE;
    features->sparseResidencyBuffer = VK_TRUE;
    features->sparseResidencyImage2D = VK_TRUE;
    features->sparseResidencyAliased = VK_TRUE;
    features->shaderResourceResidency = VK_TRUE;
    features->shaderResourceMinLod = VK_TRUE;
}

void overlay_feature_chain(ProfileSelection selection, void* pnext)
{
    if (selection.profile != Profile::Vkd3d || pnext == nullptr) {
        return;
    }

#ifdef VK_KHR_vertex_attribute_divisor
    if (auto* divisor = find_output_structure<VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR>(
            pnext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR)) {
        // Ordinary divisors are native. Only zero divisor is an unsafe claim.
        if (selection.unsafe) {
            divisor->vertexAttributeInstanceRateZeroDivisor = VK_TRUE;
        }
    }
#endif
#ifdef VK_EXT_vertex_attribute_divisor
    if (auto* divisor = find_output_structure<VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT>(
            pnext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT)) {
        // The EXT entry points and structures are bridged to native KHR support.
        divisor->vertexAttributeInstanceRateDivisor = VK_TRUE;
        if (selection.unsafe) {
            divisor->vertexAttributeInstanceRateZeroDivisor = VK_TRUE;
        }
    }
#endif

    if (selection.unsafe) {
        if (auto* robustness = find_output_structure<VkPhysicalDeviceRobustness2FeaturesEXT>(
                pnext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT)) {
            robustness->robustBufferAccess2 = VK_TRUE;
            robustness->robustImageAccess2 = VK_TRUE;
            // nullDescriptor is native and is deliberately not changed.
        }
    }
}

void overlay_properties(ProfileSelection selection, VkPhysicalDeviceProperties* properties)
{
    if (properties == nullptr || selection.profile != Profile::Vkd3d || !selection.unsafe) {
        return;
    }

    properties->sparseProperties.residencyStandard2DBlockShape = VK_TRUE;
    properties->sparseProperties.residencyStandard3DBlockShape = VK_FALSE;
    properties->sparseProperties.residencyAlignedMipSize = VK_FALSE;
    properties->sparseProperties.residencyNonResidentStrict = VK_TRUE;
}

void overlay_property_chain(ProfileSelection selection,
                            VkPhysicalDevice physical_device,
                            void* pnext,
                            PFN_vkGetPhysicalDeviceProperties2 raw_properties2)
{
    if (selection.profile != Profile::Vkd3d || pnext == nullptr) {
        return;
    }

#if defined(VK_EXT_vertex_attribute_divisor) && defined(VK_KHR_vertex_attribute_divisor)
    if (auto* ext = find_output_structure<VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT>(
            pnext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT)) {
        if (raw_properties2 != nullptr) {
            VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR khr{};
            khr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR;
            VkPhysicalDeviceProperties2 query{};
            query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            query.pNext = &khr;
            raw_properties2(physical_device, &query);
            ext->maxVertexAttribDivisor = khr.maxVertexAttribDivisor;
        }
    }
#endif

    if (!selection.unsafe) {
        return;
    }

    if (auto* vk13 = find_output_structure<VkPhysicalDeviceVulkan13Properties>(
            pnext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES)) {
        vk13->storageTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
        vk13->storageTexelBufferOffsetAlignmentBytes = 1;
    }
#ifdef VK_EXT_texel_buffer_alignment
    if (auto* alignment = find_output_structure<VkPhysicalDeviceTexelBufferAlignmentPropertiesEXT>(
            pnext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES_EXT)) {
        alignment->storageTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
        alignment->storageTexelBufferOffsetAlignmentBytes = 1;
    }
#endif
}

void overlay_queue_families(ProfileSelection selection,
                            uint32_t count,
                            VkQueueFamilyProperties* properties)
{
    if (!selection.unsafe || selection.profile != Profile::Vkd3d || properties == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (properties[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            properties[i].queueFlags |= VK_QUEUE_SPARSE_BINDING_BIT;
        }
    }
}

void overlay_queue_families2(ProfileSelection selection,
                             uint32_t count,
                             VkQueueFamilyProperties2* properties)
{
    if (properties == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        overlay_queue_families(selection, 1, &properties[i].queueFamilyProperties);
    }
}

void overlay_device_extensions(ProfileSelection selection,
                               std::vector<VkExtensionProperties>* extensions)
{
    if (extensions == nullptr || selection.profile != Profile::Vkd3d) {
        return;
    }
    if (!has_extension(*extensions, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) ||
        has_extension(*extensions, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME)) {
        return;
    }

    VkExtensionProperties alias{};
    std::strncpy(alias.extensionName, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME,
                 VK_MAX_EXTENSION_NAME_SIZE - 1);
    alias.specVersion = 3;
    extensions->push_back(alias);
}

bool DeviceCreateTransform::prepare(ProfileSelection selection,
                                    const VkDeviceCreateInfo* source,
                                    const char* const* extension_names,
                                    uint32_t extension_count)
{
    error_.clear();
    if (source == nullptr) {
        error_ = "null VkDeviceCreateInfo";
        return false;
    }

    create_info_ = *source;
    extensions_.clear();
    std::unordered_set<std::string> seen;
    for (uint32_t i = 0; i < extension_count; ++i) {
        const char* name = extension_names != nullptr ? extension_names[i] : nullptr;
        if (name == nullptr) {
            continue;
        }
        if (selection.profile == Profile::Vkd3d &&
            std::strcmp(name, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) == 0) {
            name = VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
        }
        if (seen.emplace(name).second) {
            extensions_.push_back(name);
        }
    }
    create_info_.enabledExtensionCount = static_cast<uint32_t>(extensions_.size());
    create_info_.ppEnabledExtensionNames = extensions_.empty() ? nullptr : extensions_.data();

    if (selection.profile != Profile::Vkd3d) {
        return true;
    }

    if (source->pEnabledFeatures != nullptr) {
        enabled_features_ = *source->pEnabledFeatures;
        sanitize_core_features(selection, &enabled_features_);
        create_info_.pEnabledFeatures = &enabled_features_;
    }

    if (source->pNext != nullptr) {
        if (!pnext_.clone(source->pNext)) {
            error_ = "unsupported VkDeviceCreateInfo pNext structure type " +
                     std::to_string(static_cast<int>(pnext_.unsupported_type()));
            return false;
        }
        create_info_.pNext = pnext_.head();

        if (auto* features2 = static_cast<VkPhysicalDeviceFeatures2*>(
                pnext_.find(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2))) {
            sanitize_core_features(selection, &features2->features);
        }

        if (selection.unsafe) {
            if (auto* robustness = static_cast<VkPhysicalDeviceRobustness2FeaturesEXT*>(
                    pnext_.find(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT))) {
                robustness->robustBufferAccess2 = VK_FALSE;
                robustness->robustImageAccess2 = VK_FALSE;
            }
#ifdef VK_KHR_vertex_attribute_divisor
            if (auto* divisor = static_cast<VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR*>(
                    pnext_.find(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR))) {
                divisor->vertexAttributeInstanceRateZeroDivisor = VK_FALSE;
            }
#endif
        }
#if defined(VK_EXT_vertex_attribute_divisor) && defined(VK_KHR_vertex_attribute_divisor)
        if (auto* divisor = static_cast<VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT*>(
                pnext_.find(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT))) {
            divisor->sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR;
            if (selection.unsafe) {
                divisor->vertexAttributeInstanceRateZeroDivisor = VK_FALSE;
            }
        }
#endif
    }

    return true;
}

} // namespace mali_wrapper::compatibility
