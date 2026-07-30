#pragma once

#include "compatibility/compatibility_manager.hpp"

#include <vector>
#include <vulkan/vulkan.h>

namespace mali_wrapper::compatibility::device_emulation {

void register_device(VkDevice device,
                     VkPhysicalDevice physical_device,
                     ProfileSelection selection,
                     PFN_vkGetDeviceProcAddr get_device_proc_addr,
                     const VkPhysicalDeviceMemoryProperties& memory_properties);
void unregister_device(VkDevice device);
void associate_queue(VkDevice device, VkQueue queue);

PFN_vkVoidFunction get_device_hook(VkDevice device, const char* name);

class GraphicsPipelineTransform {
public:
    void prepare(VkDevice device,
                 uint32_t create_info_count,
                 const VkGraphicsPipelineCreateInfo* source);
    const VkGraphicsPipelineCreateInfo* get() const { return transformed_; }

private:
    struct OwnedVertexInput {
        VkPipelineVertexInputStateCreateInfo vertex_input{};
        VkPipelineVertexInputDivisorStateCreateInfoKHR divisor{};
        std::vector<VkVertexInputBindingDivisorDescriptionKHR> descriptions;
    };

    std::vector<VkGraphicsPipelineCreateInfo> create_infos_;
    std::vector<OwnedVertexInput> vertex_inputs_;
    const VkGraphicsPipelineCreateInfo* transformed_ = nullptr;
};

bool unsafe_sparse_enabled(VkPhysicalDevice physical_device);
void get_sparse_image_format_properties(
    VkPhysicalDevice physical_device,
    VkFormat format,
    VkImageType type,
    VkSampleCountFlagBits samples,
    VkImageUsageFlags usage,
    VkImageTiling tiling,
    uint32_t* property_count,
    VkSparseImageFormatProperties* properties);

} // namespace mali_wrapper::compatibility::device_emulation
