#include <vulkan/vulkan.h>

#include "descriptor_fixed_array_comp_spv.hpp"
#include "descriptor_graphics_frag_spv.hpp"
#include "descriptor_graphics_vert_spv.hpp"
#include "descriptor_texel_graphics_frag_spv.hpp"
#include "descriptor_texel_graphics_vert_spv.hpp"
#include "descriptor_image_comp_spv.hpp"
#include "descriptor_mutable_comp_spv.hpp"
#include "descriptor_sampler_array_comp_spv.hpp"
#include "descriptor_storage_comp_spv.hpp"
#include "descriptor_uniform_array_comp_spv.hpp"
#include "descriptor_uniform_buffer_comp_spv.hpp"
#include "descriptor_vkd3d_array_comp_spv.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kStorageExpected = 0x13579bdfu;
constexpr uint32_t kUniformExpected = 0x2468ace0u;
constexpr uint32_t kImageExpected = 0xffbf8040u;

void check(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) {
        std::ostringstream message;
        message << operation << " failed with VkResult " << result;
        throw std::runtime_error(message.str());
    }
}

VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment)
{
    return alignment == 0 ? value : (value + alignment - 1) & ~(alignment - 1);
}

bool has_extension(VkPhysicalDevice physical_device, const char* name)
{
    uint32_t count = 0;
    check(vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr),
          "vkEnumerateDeviceExtensionProperties(count)");
    std::vector<VkExtensionProperties> extensions(count);
    check(vkEnumerateDeviceExtensionProperties(
              physical_device, nullptr, &count, extensions.data()),
          "vkEnumerateDeviceExtensionProperties(list)");
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

std::string version_string(uint32_t version)
{
    std::ostringstream value;
    value << VK_VERSION_MAJOR(version) << '.'
          << VK_VERSION_MINOR(version) << '.'
          << VK_VERSION_PATCH(version);
    return value.str();
}

struct Buffer {
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
    bool coherent = false;

    Buffer() = default;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    Buffer(Buffer&& other) noexcept
    {
        *this = std::move(other);
    }

    Buffer& operator=(Buffer&& other) noexcept
    {
        if (this != &other) {
            cleanup();
            device = other.device;
            buffer = other.buffer;
            memory = other.memory;
            address = other.address;
            size = other.size;
            mapped = other.mapped;
            coherent = other.coherent;
            other.device = VK_NULL_HANDLE;
            other.buffer = VK_NULL_HANDLE;
            other.memory = VK_NULL_HANDLE;
            other.address = 0;
            other.size = 0;
            other.mapped = nullptr;
        }
        return *this;
    }

    ~Buffer()
    {
        cleanup();
    }

    void cleanup()
    {
        if (device == VK_NULL_HANDLE) {
            return;
        }
        if (mapped != nullptr) {
            vkUnmapMemory(device, memory);
        }
        if (buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, buffer, nullptr);
        }
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, memory, nullptr);
        }
        device = VK_NULL_HANDLE;
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        mapped = nullptr;
    }
};

struct Image {
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;

    Image() = default;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    Image(Image&& other) noexcept
    {
        *this = std::move(other);
    }

    Image& operator=(Image&& other) noexcept
    {
        if (this != &other) {
            cleanup();
            device = other.device;
            image = other.image;
            memory = other.memory;
            view = other.view;
            sampler = other.sampler;
            other.device = VK_NULL_HANDLE;
            other.image = VK_NULL_HANDLE;
            other.memory = VK_NULL_HANDLE;
            other.view = VK_NULL_HANDLE;
            other.sampler = VK_NULL_HANDLE;
        }
        return *this;
    }

    ~Image()
    {
        cleanup();
    }

    void cleanup()
    {
        if (device == VK_NULL_HANDLE) {
            return;
        }
        if (sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, sampler, nullptr);
        }
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, view, nullptr);
        }
        if (image != VK_NULL_HANDLE) {
            vkDestroyImage(device, image, nullptr);
        }
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, memory, nullptr);
        }
        device = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        view = VK_NULL_HANDLE;
        sampler = VK_NULL_HANDLE;
    }
};

struct DescriptorObjects {
    VkDevice device = VK_NULL_HANDLE;
    std::vector<VkDescriptorSetLayout> set_layouts;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;

    DescriptorObjects() = default;
    DescriptorObjects(const DescriptorObjects&) = delete;
    DescriptorObjects& operator=(const DescriptorObjects&) = delete;

    DescriptorObjects(DescriptorObjects&& other) noexcept
    {
        *this = std::move(other);
    }

    DescriptorObjects& operator=(DescriptorObjects&& other) noexcept
    {
        if (this != &other) {
            cleanup();
            device = other.device;
            set_layouts = std::move(other.set_layouts);
            pipeline_layout = other.pipeline_layout;
            pipeline = other.pipeline;
            render_pass = other.render_pass;
            framebuffer = other.framebuffer;
            descriptor_pool = other.descriptor_pool;
            other.device = VK_NULL_HANDLE;
            other.set_layouts.clear();
            other.pipeline_layout = VK_NULL_HANDLE;
            other.pipeline = VK_NULL_HANDLE;
            other.render_pass = VK_NULL_HANDLE;
            other.framebuffer = VK_NULL_HANDLE;
            other.descriptor_pool = VK_NULL_HANDLE;
        }
        return *this;
    }

    ~DescriptorObjects()
    {
        cleanup();
    }

    void cleanup()
    {
        if (device == VK_NULL_HANDLE) {
            return;
        }
        if (descriptor_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        }
        if (framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        }
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, pipeline, nullptr);
        }
        if (render_pass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, render_pass, nullptr);
        }
        if (pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        }
        for (VkDescriptorSetLayout layout : set_layouts) {
            vkDestroyDescriptorSetLayout(device, layout, nullptr);
        }
        device = VK_NULL_HANDLE;
        set_layouts.clear();
        pipeline_layout = VK_NULL_HANDLE;
        pipeline = VK_NULL_HANDLE;
        render_pass = VK_NULL_HANDLE;
        framebuffer = VK_NULL_HANDLE;
        descriptor_pool = VK_NULL_HANDLE;
    }
};

class Context {
public:
    Context()
    {
        create();
    }

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    ~Context()
    {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
            if (command_pool_ != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device_, command_pool_, nullptr);
            }
            vkDestroyDevice(device_, nullptr);
        }
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
        }
    }

    VkDevice device() const { return device_; }
    VkPhysicalDevice physical_device() const { return physical_device_; }
    const VkPhysicalDeviceDescriptorBufferPropertiesEXT& descriptor_properties() const
    {
        return descriptor_properties_;
    }

    Buffer create_buffer(VkDeviceSize size,
                         VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags required_properties,
                         bool map)
    {
        Buffer result;
        result.device = device_;
        result.size = size;

        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = size;
        buffer_info.usage = usage;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device_, &buffer_info, nullptr, &result.buffer),
              "vkCreateBuffer");

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, result.buffer, &requirements);

        const uint32_t memory_type =
            find_memory_type(requirements.memoryTypeBits, required_properties);
        const VkMemoryPropertyFlags actual_properties =
            memory_properties_.memoryTypes[memory_type].propertyFlags;
        result.coherent =
            (actual_properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

        VkMemoryAllocateFlagsInfo flags_info{
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0) {
            flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        }

        VkMemoryAllocateInfo allocate_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate_info.pNext = flags_info.flags != 0 ? &flags_info : nullptr;
        allocate_info.allocationSize = requirements.size;
        allocate_info.memoryTypeIndex = memory_type;
        check(vkAllocateMemory(device_, &allocate_info, nullptr, &result.memory),
              "vkAllocateMemory(buffer)");
        check(vkBindBufferMemory(device_, result.buffer, result.memory, 0),
              "vkBindBufferMemory");

        if (map) {
            check(vkMapMemory(device_, result.memory, 0, VK_WHOLE_SIZE, 0,
                              &result.mapped),
                  "vkMapMemory(buffer)");
        }

        if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0) {
            VkBufferDeviceAddressInfo address_info{
                VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            address_info.buffer = result.buffer;
            result.address = vkGetBufferDeviceAddress(device_, &address_info);
            if (result.address == 0) {
                throw std::runtime_error(
                    "vkGetBufferDeviceAddress returned a null address");
            }
        }

        return result;
    }

    Image create_test_image()
    {
        Image result;
        result.device = device_;

        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = {1, 1, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(device_, &image_info, nullptr, &result.image),
              "vkCreateImage");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, result.image, &requirements);
        VkMemoryAllocateInfo allocate_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate_info.allocationSize = requirements.size;
        allocate_info.memoryTypeIndex =
            find_memory_type(requirements.memoryTypeBits,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device_, &allocate_info, nullptr, &result.memory),
              "vkAllocateMemory(image)");
        check(vkBindImageMemory(device_, result.image, result.memory, 0),
              "vkBindImageMemory");

        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = result.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        check(vkCreateImageView(device_, &view_info, nullptr, &result.view),
              "vkCreateImageView");

        VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler_info.magFilter = VK_FILTER_NEAREST;
        sampler_info.minFilter = VK_FILTER_NEAREST;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.maxLod = 0.0f;
        check(vkCreateSampler(device_, &sampler_info, nullptr, &result.sampler),
              "vkCreateSampler");

        submit([&](VkCommandBuffer command_buffer) {
            VkImageMemoryBarrier to_transfer{
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            to_transfer.srcAccessMask = 0;
            to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_transfer.image = result.image;
            to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            to_transfer.subresourceRange.levelCount = 1;
            to_transfer.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(command_buffer,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &to_transfer);

            VkClearColorValue color{{0.25f, 0.5f, 0.75f, 1.0f}};
            VkImageSubresourceRange range{};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.levelCount = 1;
            range.layerCount = 1;
            vkCmdClearColorImage(command_buffer, result.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &color, 1, &range);

            VkImageMemoryBarrier to_shader{
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            to_shader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_shader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_shader.image = result.image;
            to_shader.subresourceRange = range;
            vkCmdPipelineBarrier(command_buffer,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &to_shader);
        });

        return result;
    }

    Image create_render_target()
    {
        Image result;
        result.device = device_;

        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = {1, 1, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(device_, &image_info, nullptr, &result.image),
              "vkCreateImage(render target)");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, result.image, &requirements);
        VkMemoryAllocateInfo allocate_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate_info.allocationSize = requirements.size;
        allocate_info.memoryTypeIndex =
            find_memory_type(requirements.memoryTypeBits,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device_, &allocate_info, nullptr, &result.memory),
              "vkAllocateMemory(render target)");
        check(vkBindImageMemory(device_, result.image, result.memory, 0),
              "vkBindImageMemory(render target)");

        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = result.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        check(vkCreateImageView(device_, &view_info, nullptr, &result.view),
              "vkCreateImageView(render target)");
        return result;
    }

    void submit(const std::function<void(VkCommandBuffer)>& record)
    {
        VkCommandBufferAllocateInfo allocate_info{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate_info.commandPool = command_pool_;
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate_info.commandBufferCount = 1;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        check(vkAllocateCommandBuffers(device_, &allocate_info, &command_buffer),
              "vkAllocateCommandBuffers");

        VkCommandBufferBeginInfo begin_info{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command_buffer, &begin_info),
              "vkBeginCommandBuffer");
        record(command_buffer);
        check(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer");

        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        check(vkCreateFence(device_, &fence_info, nullptr, &fence),
              "vkCreateFence");

        VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer;
        check(vkQueueSubmit(queue_, 1, &submit_info, fence), "vkQueueSubmit");
        const VkResult wait_result =
            vkWaitForFences(device_, 1, &fence, VK_TRUE, 5'000'000'000ull);
        vkDestroyFence(device_, fence, nullptr);
        vkFreeCommandBuffers(device_, command_pool_, 1, &command_buffer);
        check(wait_result, "vkWaitForFences");
    }

    void flush(const Buffer& buffer)
    {
        if (buffer.coherent) {
            return;
        }
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = buffer.memory;
        range.size = VK_WHOLE_SIZE;
        check(vkFlushMappedMemoryRanges(device_, 1, &range),
              "vkFlushMappedMemoryRanges");
    }

    void invalidate(const Buffer& buffer)
    {
        if (buffer.coherent) {
            return;
        }
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = buffer.memory;
        range.size = VK_WHOLE_SIZE;
        check(vkInvalidateMappedMemoryRanges(device_, 1, &range),
              "vkInvalidateMappedMemoryRanges");
    }

    PFN_vkGetDescriptorEXT get_descriptor = nullptr;
    PFN_vkGetDescriptorSetLayoutSizeEXT get_layout_size = nullptr;
    PFN_vkGetDescriptorSetLayoutBindingOffsetEXT get_binding_offset = nullptr;
    PFN_vkCmdBindDescriptorBuffersEXT cmd_bind_descriptor_buffers = nullptr;
    PFN_vkCmdSetDescriptorBufferOffsetsEXT cmd_set_descriptor_offsets = nullptr;
    PFN_vkCmdPushDescriptorSetKHR cmd_push_descriptor_set = nullptr;

private:
    void create()
    {
        VkApplicationInfo application_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application_info.pApplicationName = "Mali descriptor-buffer diagnostic";
        application_info.pEngineName = "native-vulkan-diagnostic";
        application_info.apiVersion = VK_API_VERSION_1_2;

        VkInstanceCreateInfo instance_info{
            VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instance_info.pApplicationInfo = &application_info;
        check(vkCreateInstance(&instance_info, nullptr, &instance_),
              "vkCreateInstance");

        uint32_t physical_device_count = 0;
        check(vkEnumeratePhysicalDevices(instance_, &physical_device_count, nullptr),
              "vkEnumeratePhysicalDevices(count)");
        if (physical_device_count == 0) {
            throw std::runtime_error("No Vulkan physical devices were found");
        }
        std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
        check(vkEnumeratePhysicalDevices(
                  instance_, &physical_device_count, physical_devices.data()),
              "vkEnumeratePhysicalDevices(list)");
        physical_device_ = physical_devices.front();

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_device_, &properties);
        std::cout << "Device: " << properties.deviceName
                  << " vendor=0x" << std::hex << properties.vendorID << std::dec
                  << " driver=" << version_string(properties.driverVersion)
                  << " api=" << version_string(properties.apiVersion) << '\n';

        if (!has_extension(physical_device_,
                           VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME)) {
            throw std::runtime_error(
                "VK_EXT_descriptor_buffer is not advertised");
        }
        if (!has_extension(physical_device_,
                           VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME)) {
            throw std::runtime_error(
                "VK_EXT_mutable_descriptor_type is not advertised");
        }
        if (!has_extension(physical_device_,
                           VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
            throw std::runtime_error(
                "VK_KHR_push_descriptor is not advertised");
        }

        VkPhysicalDeviceVulkan12Features vulkan_1_2_features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutable_features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
        VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor_features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
        descriptor_features.pNext = &mutable_features;
        mutable_features.pNext = &vulkan_1_2_features;

        descriptor_properties_ = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 properties2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = &descriptor_properties_;
        vkGetPhysicalDeviceProperties2(physical_device_, &properties2);

        VkPhysicalDeviceFeatures2 features2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.pNext = &descriptor_features;
        vkGetPhysicalDeviceFeatures2(physical_device_, &features2);
        if (!descriptor_features.descriptorBuffer) {
            throw std::runtime_error(
                "descriptorBuffer feature is not supported");
        }
        if (!descriptor_features.descriptorBufferPushDescriptors) {
            throw std::runtime_error(
                "descriptorBufferPushDescriptors is not supported");
        }
        if (!mutable_features.mutableDescriptorType) {
            throw std::runtime_error(
                "mutableDescriptorType feature is not supported");
        }
        if (!vulkan_1_2_features.bufferDeviceAddress) {
            throw std::runtime_error(
                "bufferDeviceAddress feature is not supported");
        }
        if (!vulkan_1_2_features.descriptorBindingVariableDescriptorCount ||
            !vulkan_1_2_features.runtimeDescriptorArray) {
            throw std::runtime_error(
                "Required descriptor-indexing features are not supported");
        }

        std::cout
            << "Descriptor properties: alignment="
            << descriptor_properties_.descriptorBufferOffsetAlignment
            << " storage=" << descriptor_properties_.storageBufferDescriptorSize
            << " sampled-image="
            << descriptor_properties_.sampledImageDescriptorSize
            << " sampler=" << descriptor_properties_.samplerDescriptorSize
            << " push-descriptors="
            << (descriptor_features.descriptorBufferPushDescriptors
                    ? "yes" : "no")
            << " bufferless-push="
            << (descriptor_properties_.bufferlessPushDescriptors
                    ? "yes" : "no")
            << " max-bindings="
            << descriptor_properties_.maxDescriptorBufferBindings << '\n';

        uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(
            physical_device_, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(
            physical_device_, &queue_family_count, queue_families.data());
        for (uint32_t i = 0; i < queue_family_count; ++i) {
            const VkQueueFlags required =
                VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT;
            if ((queue_families[i].queueFlags & required) == required) {
                queue_family_ = i;
                break;
            }
        }
        if (queue_family_ == UINT32_MAX) {
            throw std::runtime_error(
                "No graphics-and-compute-capable queue family found");
        }

        descriptor_features.descriptorBuffer = VK_TRUE;
        descriptor_features.descriptorBufferCaptureReplay = VK_FALSE;
        descriptor_features.descriptorBufferImageLayoutIgnored = VK_FALSE;
        descriptor_features.descriptorBufferPushDescriptors = VK_TRUE;
        mutable_features.mutableDescriptorType = VK_TRUE;
        vulkan_1_2_features.bufferDeviceAddress = VK_TRUE;
        vulkan_1_2_features.bufferDeviceAddressCaptureReplay = VK_FALSE;
        vulkan_1_2_features.bufferDeviceAddressMultiDevice = VK_FALSE;
        vulkan_1_2_features.descriptorIndexing = VK_TRUE;
        vulkan_1_2_features.runtimeDescriptorArray = VK_TRUE;
        vulkan_1_2_features.descriptorBindingVariableDescriptorCount = VK_TRUE;

        const float queue_priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = queue_family_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;

        const char* extensions[] = {
            VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
            VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME,
            VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        };
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.pNext = &descriptor_features;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount =
            static_cast<uint32_t>(std::size(extensions));
        device_info.ppEnabledExtensionNames = extensions;
        check(vkCreateDevice(physical_device_, &device_info, nullptr, &device_),
              "vkCreateDevice");

        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
        vkGetPhysicalDeviceMemoryProperties(
            physical_device_, &memory_properties_);

        VkCommandPoolCreateInfo pool_info{
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = queue_family_;
        check(vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_),
              "vkCreateCommandPool");

        get_descriptor = reinterpret_cast<PFN_vkGetDescriptorEXT>(
            vkGetDeviceProcAddr(device_, "vkGetDescriptorEXT"));
        get_layout_size =
            reinterpret_cast<PFN_vkGetDescriptorSetLayoutSizeEXT>(
                vkGetDeviceProcAddr(device_,
                                    "vkGetDescriptorSetLayoutSizeEXT"));
        get_binding_offset =
            reinterpret_cast<PFN_vkGetDescriptorSetLayoutBindingOffsetEXT>(
                vkGetDeviceProcAddr(
                    device_, "vkGetDescriptorSetLayoutBindingOffsetEXT"));
        cmd_bind_descriptor_buffers =
            reinterpret_cast<PFN_vkCmdBindDescriptorBuffersEXT>(
                vkGetDeviceProcAddr(device_,
                                    "vkCmdBindDescriptorBuffersEXT"));
        cmd_set_descriptor_offsets =
            reinterpret_cast<PFN_vkCmdSetDescriptorBufferOffsetsEXT>(
                vkGetDeviceProcAddr(
                    device_, "vkCmdSetDescriptorBufferOffsetsEXT"));
        cmd_push_descriptor_set =
            reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
                vkGetDeviceProcAddr(device_, "vkCmdPushDescriptorSetKHR"));
        if (!get_descriptor || !get_layout_size || !get_binding_offset ||
            !cmd_bind_descriptor_buffers || !cmd_set_descriptor_offsets ||
            !cmd_push_descriptor_set) {
            throw std::runtime_error(
                "One or more descriptor-buffer entry points are null");
        }
    }

    uint32_t find_memory_type(uint32_t type_bits,
                              VkMemoryPropertyFlags required) const
    {
        for (uint32_t i = 0; i < memory_properties_.memoryTypeCount; ++i) {
            if ((type_bits & (1u << i)) != 0 &&
                (memory_properties_.memoryTypes[i].propertyFlags & required) ==
                    required) {
                return i;
            }
        }
        std::ostringstream message;
        message << "No memory type satisfies flags 0x"
                << std::hex << required << " and type bits 0x" << type_bits;
        throw std::runtime_error(message.str());
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = UINT32_MAX;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_properties_{};
    VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptor_properties_{};
};

VkShaderModule create_shader_module(VkDevice device,
                                    const uint32_t* code,
                                    size_t size)
{
    VkShaderModuleCreateInfo create_info{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    create_info.codeSize = size;
    create_info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &create_info, nullptr, &module),
          "vkCreateShaderModule");
    return module;
}

VkDescriptorSetLayout create_set_layout(
    VkDevice device,
    const std::vector<VkDescriptorSetLayoutBinding>& bindings,
    bool descriptor_buffer)
{
    VkDescriptorSetLayoutCreateInfo create_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    create_info.flags = descriptor_buffer
        ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT
        : 0;
    create_info.bindingCount = static_cast<uint32_t>(bindings.size());
    create_info.pBindings = bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(device, &create_info, nullptr, &layout),
          "vkCreateDescriptorSetLayout");
    return layout;
}

DescriptorObjects create_compute_objects(
    Context& context,
    std::vector<VkDescriptorSetLayout> set_layouts,
    const uint32_t* shader_code,
    size_t shader_size,
    bool descriptor_buffer,
    uint32_t push_constant_size = 0)
{
    DescriptorObjects objects;
    objects.device = context.device();
    objects.set_layouts = std::move(set_layouts);

    VkPipelineLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount =
        static_cast<uint32_t>(objects.set_layouts.size());
    layout_info.pSetLayouts = objects.set_layouts.data();
    VkPushConstantRange push_constant_range{};
    if (push_constant_size != 0) {
        push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push_constant_range.size = push_constant_size;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push_constant_range;
    }
    check(vkCreatePipelineLayout(context.device(), &layout_info, nullptr,
                                 &objects.pipeline_layout),
          "vkCreatePipelineLayout");

    VkShaderModule shader =
        create_shader_module(context.device(), shader_code, shader_size);
    VkPipelineShaderStageCreateInfo stage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipeline_info{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_info.flags = descriptor_buffer
        ? VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT
        : 0;
    pipeline_info.stage = stage;
    pipeline_info.layout = objects.pipeline_layout;
    const VkResult pipeline_result =
        vkCreateComputePipelines(context.device(), VK_NULL_HANDLE, 1,
                                 &pipeline_info, nullptr, &objects.pipeline);
    vkDestroyShaderModule(context.device(), shader, nullptr);
    check(pipeline_result, "vkCreateComputePipelines");
    return objects;
}

DescriptorObjects create_graphics_objects(
    Context& context,
    std::vector<VkDescriptorSetLayout> set_layouts,
    const Image& render_target,
    const uint32_t* vertex_shader_code = descriptor_graphics_vert_spv,
    size_t vertex_shader_size = sizeof(descriptor_graphics_vert_spv),
    const uint32_t* fragment_shader_code = descriptor_graphics_frag_spv,
    size_t fragment_shader_size = sizeof(descriptor_graphics_frag_spv),
    VkShaderStageFlags push_constant_stages = VK_SHADER_STAGE_FRAGMENT_BIT,
    uint32_t push_constant_size = sizeof(uint32_t))
{
    DescriptorObjects objects;
    objects.device = context.device();
    objects.set_layouts = std::move(set_layouts);

    VkPushConstantRange push_constant_range{};
    push_constant_range.stageFlags = push_constant_stages;
    push_constant_range.size = push_constant_size;
    VkPipelineLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount =
        static_cast<uint32_t>(objects.set_layouts.size());
    layout_info.pSetLayouts = objects.set_layouts.data();
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_constant_range;
    check(vkCreatePipelineLayout(context.device(), &layout_info, nullptr,
                                 &objects.pipeline_layout),
          "vkCreatePipelineLayout(graphics)");

    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color_reference{
        0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo render_pass_info{
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = 1;
    render_pass_info.pDependencies = &dependency;
    check(vkCreateRenderPass(context.device(), &render_pass_info, nullptr,
                             &objects.render_pass),
          "vkCreateRenderPass");

    VkFramebufferCreateInfo framebuffer_info{
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer_info.renderPass = objects.render_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &render_target.view;
    framebuffer_info.width = 1;
    framebuffer_info.height = 1;
    framebuffer_info.layers = 1;
    check(vkCreateFramebuffer(context.device(), &framebuffer_info, nullptr,
                              &objects.framebuffer),
          "vkCreateFramebuffer");

    const std::array<VkShaderModule, 2> modules{{
        create_shader_module(context.device(), vertex_shader_code,
                             vertex_shader_size),
        create_shader_module(context.device(), fragment_shader_code,
                             fragment_shader_size),
    }};
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = modules[0];
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = modules[1];
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {1, 1}};
    VkPipelineViewportStateCreateInfo viewport_state{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rasterization{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    VkGraphicsPipelineCreateInfo pipeline_info{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    pipeline_info.stageCount = static_cast<uint32_t>(stages.size());
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.layout = objects.pipeline_layout;
    pipeline_info.renderPass = objects.render_pass;
    pipeline_info.subpass = 0;
    const VkResult pipeline_result = vkCreateGraphicsPipelines(
        context.device(), VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
        &objects.pipeline);
    for (VkShaderModule module : modules) {
        vkDestroyShaderModule(context.device(), module, nullptr);
    }
    check(pipeline_result, "vkCreateGraphicsPipelines");
    return objects;
}

void reset_output(Context& context, Buffer& output)
{
    std::memset(output.mapped, 0, sizeof(uint32_t));
    context.flush(output);
}

uint32_t read_output(Context& context, Buffer& output)
{
    context.invalidate(output);
    uint32_t value = 0;
    std::memcpy(&value, output.mapped, sizeof(value));
    return value;
}

void add_compute_to_host_barrier(VkCommandBuffer command_buffer,
                                 VkBuffer output)
{
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = output;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command_buffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
}

void print_payload(const char* name, const Buffer& descriptor_buffer,
                   VkDeviceSize offset, size_t size)
{
    const auto* bytes =
        static_cast<const uint8_t*>(descriptor_buffer.mapped) + offset;
    std::cout << "  " << name << " payload @" << offset << ':';
    for (size_t i = 0; i < size; ++i) {
        std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(bytes[i]);
    }
    std::cout << std::dec << std::setfill(' ') << '\n';
}

bool report_result(const char* name, uint32_t actual, uint32_t expected)
{
    const bool passed = actual == expected;
    std::cout << (passed ? "PASS" : "FAIL") << ": " << name
              << " expected=0x" << std::hex << std::setw(8)
              << std::setfill('0') << expected
              << " actual=0x" << std::setw(8) << actual
              << std::dec << std::setfill(' ') << '\n';
    return passed;
}

bool test_storage_descriptor_set(Context& context, Buffer& output)
{
    const std::vector<VkDescriptorSetLayoutBinding> bindings{{
        0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr,
    }};
    std::vector<VkDescriptorSetLayout> layouts{
        create_set_layout(context.device(), bindings, false),
    };
    DescriptorObjects objects = create_compute_objects(
        context, std::move(layouts), descriptor_storage_comp_spv,
        sizeof(descriptor_storage_comp_spv), false);

    VkDescriptorPoolSize pool_size{
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    check(vkCreateDescriptorPool(context.device(), &pool_info, nullptr,
                                 &objects.descriptor_pool),
          "vkCreateDescriptorPool(storage)");

    VkDescriptorSetAllocateInfo allocate_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate_info.descriptorPool = objects.descriptor_pool;
    allocate_info.descriptorSetCount = 1;
    allocate_info.pSetLayouts = objects.set_layouts.data();
    VkDescriptorSet set = VK_NULL_HANDLE;
    check(vkAllocateDescriptorSets(context.device(), &allocate_info, &set),
          "vkAllocateDescriptorSets(storage)");

    VkDescriptorBufferInfo buffer_info{output.buffer, 0, sizeof(uint32_t)};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(context.device(), 1, &write, 0, nullptr);

    reset_output(context, output);
    context.submit([&](VkCommandBuffer command_buffer) {
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          objects.pipeline);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                objects.pipeline_layout, 0, 1, &set,
                                0, nullptr);
        vkCmdDispatch(command_buffer, 1, 1, 1);
        add_compute_to_host_barrier(command_buffer, output.buffer);
    });
    return report_result("descriptor-set storage buffer",
                         read_output(context, output), kStorageExpected);
}

bool test_storage_descriptor_buffer(Context& context, Buffer& output)
{
    const std::vector<VkDescriptorSetLayoutBinding> bindings{{
        0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr,
    }};
    std::vector<VkDescriptorSetLayout> layouts{
        create_set_layout(context.device(), bindings, true),
    };
    DescriptorObjects objects = create_compute_objects(
        context, std::move(layouts), descriptor_storage_comp_spv,
        sizeof(descriptor_storage_comp_spv), true);

    VkDeviceSize layout_size = 0;
    VkDeviceSize binding_offset = 0;
    context.get_layout_size(context.device(), objects.set_layouts[0],
                            &layout_size);
    context.get_binding_offset(context.device(), objects.set_layouts[0], 0,
                               &binding_offset);
    const auto& properties = context.descriptor_properties();
    const VkDeviceSize allocation_size = align_up(
        std::max(layout_size,
                 binding_offset + properties.storageBufferDescriptorSize),
        properties.descriptorBufferOffsetAlignment);
    Buffer descriptor_buffer = context.create_buffer(
        allocation_size,
        VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, true);
    std::memset(descriptor_buffer.mapped, 0,
                static_cast<size_t>(allocation_size));

    VkDescriptorAddressInfoEXT address_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
    address_info.address = output.address;
    address_info.range = sizeof(uint32_t);
    address_info.format = VK_FORMAT_UNDEFINED;
    VkDescriptorGetInfoEXT descriptor_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    descriptor_info.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptor_info.data.pStorageBuffer = &address_info;
    context.get_descriptor(
        context.device(), &descriptor_info,
        properties.storageBufferDescriptorSize,
        static_cast<uint8_t*>(descriptor_buffer.mapped) + binding_offset);
    context.flush(descriptor_buffer);

    std::cout << "Storage descriptor-buffer layout: size=" << layout_size
              << " binding-offset=" << binding_offset
              << " allocation=" << allocation_size << '\n';
    print_payload("storage", descriptor_buffer, binding_offset,
                  properties.storageBufferDescriptorSize);

    reset_output(context, output);
    context.submit([&](VkCommandBuffer command_buffer) {
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          objects.pipeline);
        VkDescriptorBufferBindingInfoEXT binding{
            VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT};
        binding.address = descriptor_buffer.address;
        binding.usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;
        context.cmd_bind_descriptor_buffers(command_buffer, 1, &binding);
        const uint32_t buffer_index = 0;
        const VkDeviceSize offset = 0;
        context.cmd_set_descriptor_offsets(
            command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            objects.pipeline_layout, 0, 1, &buffer_index, &offset);
        vkCmdDispatch(command_buffer, 1, 1, 1);
        add_compute_to_host_barrier(command_buffer, output.buffer);
    });
    return report_result("descriptor-buffer storage buffer",
                         read_output(context, output), kStorageExpected);
}

bool test_uniform_buffer(Context& context, Buffer& output,
                         bool descriptor_buffer)
{
    const std::vector<VkDescriptorSetLayoutBinding> bindings{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
         VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
         VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    std::vector<VkDescriptorSetLayout> layouts{
        create_set_layout(context.device(), bindings, descriptor_buffer),
    };
    DescriptorObjects objects = create_compute_objects(
        context, std::move(layouts), descriptor_uniform_buffer_comp_spv,
        sizeof(descriptor_uniform_buffer_comp_spv), descriptor_buffer);

    Buffer input = context.create_buffer(
        16,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, true);
    std::memset(input.mapped, 0, 16);
    std::memcpy(input.mapped, &kUniformExpected, sizeof(kUniformExpected));
    context.flush(input);

    reset_output(context, output);
    if (!descriptor_buffer) {
        const std::array<VkDescriptorPoolSize, 2> pool_sizes{{
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
        }};
        VkDescriptorPoolCreateInfo pool_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.maxSets = 1;
        pool_info.poolSizeCount =
            static_cast<uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        check(vkCreateDescriptorPool(context.device(), &pool_info, nullptr,
                                     &objects.descriptor_pool),
              "vkCreateDescriptorPool(uniform)");

        VkDescriptorSetAllocateInfo allocate_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate_info.descriptorPool = objects.descriptor_pool;
        allocate_info.descriptorSetCount = 1;
        allocate_info.pSetLayouts = objects.set_layouts.data();
        VkDescriptorSet set = VK_NULL_HANDLE;
        check(vkAllocateDescriptorSets(context.device(), &allocate_info, &set),
              "vkAllocateDescriptorSets(uniform)");

        VkDescriptorBufferInfo input_info{input.buffer, 0, 16};
        VkDescriptorBufferInfo output_info{
            output.buffer, 0, sizeof(uint32_t)};
        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &input_info;
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &output_info;
        vkUpdateDescriptorSets(context.device(),
                               static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);

        context.submit([&](VkCommandBuffer command_buffer) {
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                              objects.pipeline);
            vkCmdBindDescriptorSets(
                command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                objects.pipeline_layout, 0, 1, &set, 0, nullptr);
            vkCmdDispatch(command_buffer, 1, 1, 1);
            add_compute_to_host_barrier(command_buffer, output.buffer);
        });
    } else {
        const auto& properties = context.descriptor_properties();
        VkDeviceSize layout_size = 0;
        VkDeviceSize uniform_offset = 0;
        VkDeviceSize storage_offset = 0;
        context.get_layout_size(context.device(), objects.set_layouts[0],
                                &layout_size);
        context.get_binding_offset(context.device(), objects.set_layouts[0],
                                   0, &uniform_offset);
        context.get_binding_offset(context.device(), objects.set_layouts[0],
                                   1, &storage_offset);
        const VkDeviceSize allocation_size = align_up(
            std::max({layout_size,
                      uniform_offset +
                          properties.uniformBufferDescriptorSize,
                      storage_offset +
                          properties.storageBufferDescriptorSize}),
            properties.descriptorBufferOffsetAlignment);
        Buffer descriptors = context.create_buffer(
            allocation_size,
            VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, true);
        std::memset(descriptors.mapped, 0,
                    static_cast<size_t>(allocation_size));

        VkDescriptorAddressInfoEXT input_address{
            VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
        input_address.address = input.address;
        input_address.range = 16;
        input_address.format = VK_FORMAT_UNDEFINED;
        VkDescriptorGetInfoEXT uniform_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
        uniform_info.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uniform_info.data.pUniformBuffer = &input_address;
        context.get_descriptor(
            context.device(), &uniform_info,
            properties.uniformBufferDescriptorSize,
            static_cast<uint8_t*>(descriptors.mapped) + uniform_offset);

        VkDescriptorAddressInfoEXT output_address{
            VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
        output_address.address = output.address;
        output_address.range = sizeof(uint32_t);
        output_address.format = VK_FORMAT_UNDEFINED;
        VkDescriptorGetInfoEXT storage_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
        storage_info.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        storage_info.data.pStorageBuffer = &output_address;
        context.get_descriptor(
            context.device(), &storage_info,
            properties.storageBufferDescriptorSize,
            static_cast<uint8_t*>(descriptors.mapped) + storage_offset);
        context.flush(descriptors);

        context.submit([&](VkCommandBuffer command_buffer) {
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                              objects.pipeline);
            VkDescriptorBufferBindingInfoEXT binding{
                VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT};
            binding.address = descriptors.address;
            binding.usage =
                VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;
            context.cmd_bind_descriptor_buffers(command_buffer, 1, &binding);
            const uint32_t buffer_index = 0;
            const VkDeviceSize offset = 0;
            context.cmd_set_descriptor_offsets(
                command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                objects.pipeline_layout, 0, 1, &buffer_index, &offset);
            vkCmdDispatch(command_buffer, 1, 1, 1);
            add_compute_to_host_barrier(command_buffer, output.buffer);
        });
    }

    return report_result(
        descriptor_buffer ? "descriptor-buffer uniform buffer"
                          : "descriptor-set uniform buffer",
        read_output(context, output), kUniformExpected);
}

bool test_descriptor_buffer_push_descriptor(Context& context, Buffer& output)
{
    const VkDescriptorSetLayoutBinding binding{
        0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr,
    };
    VkDescriptorSetLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.flags =
        VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR |
        VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(context.device(), &layout_info, nullptr,
                                      &layout),
          "vkCreateDescriptorSetLayout(push descriptor buffer)");

    DescriptorObjects objects = create_compute_objects(
        context, {layout}, descriptor_storage_comp_spv,
        sizeof(descriptor_storage_comp_spv), true);

    Buffer push_descriptor_backing = context.create_buffer(
        4096,
        VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_PUSH_DESCRIPTORS_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, true);
    std::memset(push_descriptor_backing.mapped, 0,
                static_cast<size_t>(push_descriptor_backing.size));
    context.flush(push_descriptor_backing);

    VkDescriptorBufferInfo output_info{
        output.buffer, 0, sizeof(uint32_t)};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &output_info;

    VkDescriptorBufferBindingPushDescriptorBufferHandleEXT push_handle{
        VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_PUSH_DESCRIPTOR_BUFFER_HANDLE_EXT};
    push_handle.buffer = push_descriptor_backing.buffer;
    VkDescriptorBufferBindingInfoEXT binding_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT};
    binding_info.pNext = &push_handle;
    binding_info.address = push_descriptor_backing.address;
    binding_info.usage =
        VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
        VK_BUFFER_USAGE_PUSH_DESCRIPTORS_DESCRIPTOR_BUFFER_BIT_EXT;

    reset_output(context, output);
    context.submit([&](VkCommandBuffer command_buffer) {
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          objects.pipeline);
        context.cmd_bind_descriptor_buffers(command_buffer, 1, &binding_info);
        context.cmd_push_descriptor_set(
            command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            objects.pipeline_layout, 0, 1, &write);
        vkCmdDispatch(command_buffer, 1, 1, 1);
        add_compute_to_host_barrier(command_buffer, output.buffer);
    });
    return report_result("descriptor-buffer backed push descriptor",
                         read_output(context, output), kStorageExpected);
}

struct ImageLayouts {
    std::vector<VkDescriptorSetLayout> layouts;
    VkDescriptorSetLayout resource = VK_NULL_HANDLE;
    VkDescriptorSetLayout sampler = VK_NULL_HANDLE;
};

ImageLayouts create_image_layouts(Context& context, bool descriptor_buffer)
{
    const std::vector<VkDescriptorSetLayoutBinding> resource_bindings{
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
         VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
         VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    const std::vector<VkDescriptorSetLayoutBinding> sampler_bindings{
        {0, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
         VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    ImageLayouts result;
    result.resource =
        create_set_layout(context.device(), resource_bindings,
                          descriptor_buffer);
    result.sampler =
        create_set_layout(context.device(), sampler_bindings,
                          descriptor_buffer);
    result.layouts = {result.resource, result.sampler};
    return result;
}

bool test_image_descriptor_set(Context& context, Buffer& output,
                               const Image& image)
{
    ImageLayouts layouts = create_image_layouts(context, false);
    DescriptorObjects objects = create_compute_objects(
        context, std::move(layouts.layouts), descriptor_image_comp_spv,
        sizeof(descriptor_image_comp_spv), false);

    const std::array<VkDescriptorPoolSize, 3> pool_sizes{{
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1},
    }};
    VkDescriptorPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 2;
    pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();
    check(vkCreateDescriptorPool(context.device(), &pool_info, nullptr,
                                 &objects.descriptor_pool),
          "vkCreateDescriptorPool(image)");

    VkDescriptorSetAllocateInfo allocate_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate_info.descriptorPool = objects.descriptor_pool;
    allocate_info.descriptorSetCount = 2;
    allocate_info.pSetLayouts = objects.set_layouts.data();
    std::array<VkDescriptorSet, 2> sets{};
    check(vkAllocateDescriptorSets(context.device(), &allocate_info,
                                   sets.data()),
          "vkAllocateDescriptorSets(image)");

    VkDescriptorImageInfo image_info{
        VK_NULL_HANDLE, image.view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo output_info{
        output.buffer, 0, sizeof(uint32_t)};
    VkDescriptorImageInfo sampler_info{
        image.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    std::array<VkWriteDescriptorSet, 3> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet = sets[0];
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo = &image_info;
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[1].dstSet = sets[0];
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &output_info;
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[2].dstSet = sets[1];
    writes[2].dstBinding = 0;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[2].pImageInfo = &sampler_info;
    vkUpdateDescriptorSets(context.device(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);

    reset_output(context, output);
    context.submit([&](VkCommandBuffer command_buffer) {
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          objects.pipeline);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                objects.pipeline_layout, 0, 2, sets.data(),
                                0, nullptr);
        vkCmdDispatch(command_buffer, 1, 1, 1);
        add_compute_to_host_barrier(command_buffer, output.buffer);
    });
    return report_result("descriptor-set sampled image + sampler",
                         read_output(context, output), kImageExpected);
}

bool test_image_descriptor_buffer(Context& context, Buffer& output,
                                  const Image& image)
{
    ImageLayouts layouts = create_image_layouts(context, true);
    DescriptorObjects objects = create_compute_objects(
        context, std::move(layouts.layouts), descriptor_image_comp_spv,
        sizeof(descriptor_image_comp_spv), true);

    const auto& properties = context.descriptor_properties();
    VkDeviceSize resource_layout_size = 0;
    VkDeviceSize sampled_image_offset = 0;
    VkDeviceSize storage_buffer_offset = 0;
    VkDeviceSize sampler_layout_size = 0;
    VkDeviceSize sampler_offset = 0;
    context.get_layout_size(context.device(), objects.set_layouts[0],
                            &resource_layout_size);
    context.get_binding_offset(context.device(), objects.set_layouts[0], 0,
                               &sampled_image_offset);
    context.get_binding_offset(context.device(), objects.set_layouts[0], 1,
                               &storage_buffer_offset);
    context.get_layout_size(context.device(), objects.set_layouts[1],
                            &sampler_layout_size);
    context.get_binding_offset(context.device(), objects.set_layouts[1], 0,
                               &sampler_offset);

    const VkDeviceSize resource_allocation_size = align_up(
        std::max({resource_layout_size,
                  sampled_image_offset +
                      properties.sampledImageDescriptorSize,
                  storage_buffer_offset +
                      properties.storageBufferDescriptorSize}),
        properties.descriptorBufferOffsetAlignment);
    const VkDeviceSize sampler_allocation_size = align_up(
        std::max(sampler_layout_size,
                 sampler_offset + properties.samplerDescriptorSize),
        properties.descriptorBufferOffsetAlignment);

    Buffer resource_descriptors = context.create_buffer(
        resource_allocation_size,
        VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, true);
    Buffer sampler_descriptors = context.create_buffer(
        sampler_allocation_size,
        VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, true);
    std::memset(resource_descriptors.mapped, 0,
                static_cast<size_t>(resource_allocation_size));
    std::memset(sampler_descriptors.mapped, 0,
                static_cast<size_t>(sampler_allocation_size));

    VkDescriptorImageInfo image_info{
        VK_NULL_HANDLE, image.view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorGetInfoEXT sampled_image_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    sampled_image_info.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sampled_image_info.data.pSampledImage = &image_info;
    context.get_descriptor(
        context.device(), &sampled_image_info,
        properties.sampledImageDescriptorSize,
        static_cast<uint8_t*>(resource_descriptors.mapped) +
            sampled_image_offset);

    VkDescriptorAddressInfoEXT output_address{
        VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
    output_address.address = output.address;
    output_address.range = sizeof(uint32_t);
    output_address.format = VK_FORMAT_UNDEFINED;
    VkDescriptorGetInfoEXT storage_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    storage_info.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    storage_info.data.pStorageBuffer = &output_address;
    context.get_descriptor(
        context.device(), &storage_info,
        properties.storageBufferDescriptorSize,
        static_cast<uint8_t*>(resource_descriptors.mapped) +
            storage_buffer_offset);

    VkDescriptorGetInfoEXT sampler_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    sampler_info.type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sampler_info.data.pSampler = &image.sampler;
    context.get_descriptor(
        context.device(), &sampler_info,
        properties.samplerDescriptorSize,
        static_cast<uint8_t*>(sampler_descriptors.mapped) + sampler_offset);
    context.flush(resource_descriptors);
    context.flush(sampler_descriptors);

    std::cout << "Image descriptor-buffer layouts: resource-size="
              << resource_layout_size
              << " sampled-offset=" << sampled_image_offset
              << " storage-offset=" << storage_buffer_offset
              << " sampler-size=" << sampler_layout_size
              << " sampler-offset=" << sampler_offset << '\n';
    print_payload("sampled-image", resource_descriptors,
                  sampled_image_offset,
                  properties.sampledImageDescriptorSize);
    print_payload("storage", resource_descriptors, storage_buffer_offset,
                  properties.storageBufferDescriptorSize);
    print_payload("sampler", sampler_descriptors, sampler_offset,
                  properties.samplerDescriptorSize);

    reset_output(context, output);
    context.submit([&](VkCommandBuffer command_buffer) {
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          objects.pipeline);
        const std::array<VkDescriptorBufferBindingInfoEXT, 2> bindings{{
            {VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT, nullptr,
             resource_descriptors.address,
             VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT},
            {VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT, nullptr,
             sampler_descriptors.address,
             VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT},
        }};
        context.cmd_bind_descriptor_buffers(
            command_buffer, static_cast<uint32_t>(bindings.size()),
            bindings.data());
        const std::array<uint32_t, 2> buffer_indices{{0, 1}};
        const std::array<VkDeviceSize, 2> offsets{{0, 0}};
        context.cmd_set_descriptor_offsets(
            command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            objects.pipeline_layout, 0,
            static_cast<uint32_t>(buffer_indices.size()),
            buffer_indices.data(), offsets.data());
        vkCmdDispatch(command_buffer, 1, 1, 1);
        add_compute_to_host_barrier(command_buffer, output.buffer);
    });
    return report_result("descriptor-buffer sampled image + sampler",
                         read_output(context, output), kImageExpected);
}

constexpr uint32_t kMutableDescriptorCount = 1024;
constexpr uint32_t kMutableDescriptorIndex = 511;

const std::array<VkDescriptorType, 6> kMutableTypes{{
    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
    VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
}};

VkDescriptorSetLayout create_sampled_array_layout(Context& context,
                                                  bool descriptor_buffer,
                                                  bool mutable_descriptor,
                                                  uint32_t descriptor_count,
                                                  bool variable_descriptor_count,
                                                  bool vkd3d_aux_binding)
{
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    if (vkd3d_aux_binding) {
        VkDescriptorSetLayoutBinding aux_binding{};
        aux_binding.binding = 0;
        aux_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        aux_binding.descriptorCount = 1;
        aux_binding.stageFlags = VK_SHADER_STAGE_ALL;
        bindings.push_back(aux_binding);
    }

    VkDescriptorSetLayoutBinding array_binding{};
    array_binding.binding = vkd3d_aux_binding ? 1 : 0;
    array_binding.descriptorType = mutable_descriptor
        ? VK_DESCRIPTOR_TYPE_MUTABLE_EXT
        : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    array_binding.descriptorCount = descriptor_count;
    array_binding.stageFlags = VK_SHADER_STAGE_ALL;
    bindings.push_back(array_binding);

    std::vector<VkDescriptorBindingFlags> binding_flags(bindings.size(), 0);
    if (variable_descriptor_count) {
        binding_flags.back() =
            VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    }
    std::vector<VkMutableDescriptorTypeListEXT> mutable_lists(bindings.size());
    mutable_lists.back().descriptorTypeCount =
        static_cast<uint32_t>(kMutableTypes.size());
    mutable_lists.back().pDescriptorTypes = kMutableTypes.data();
    VkMutableDescriptorTypeCreateInfoEXT mutable_info{
        VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT};
    mutable_info.mutableDescriptorTypeListCount =
        static_cast<uint32_t>(mutable_lists.size());
    mutable_info.pMutableDescriptorTypeLists = mutable_lists.data();
    VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    binding_flags_info.pNext =
        mutable_descriptor ? &mutable_info : nullptr;
    binding_flags_info.bindingCount =
        static_cast<uint32_t>(binding_flags.size());
    binding_flags_info.pBindingFlags = binding_flags.data();

    VkDescriptorSetLayoutCreateInfo create_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    create_info.pNext = &binding_flags_info;
    create_info.flags = descriptor_buffer
        ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT
        : 0;
    create_info.bindingCount = static_cast<uint32_t>(bindings.size());
    create_info.pBindings = bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(context.device(), &create_info,
                                      nullptr, &layout),
          "vkCreateDescriptorSetLayout(mutable)");
    return layout;
}

std::vector<VkDescriptorSetLayout> create_mutable_test_layouts(
    Context& context, bool descriptor_buffer,
    bool mutable_descriptor = true,
    uint32_t descriptor_count = kMutableDescriptorCount,
    bool variable_descriptor_count = true,
    bool vkd3d_aux_binding = false)
{
    const std::vector<VkDescriptorSetLayoutBinding> sampler_bindings{{
        0, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr,
    }};
    const std::vector<VkDescriptorSetLayoutBinding> storage_bindings{{
        0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr,
    }};
    return {
        create_sampled_array_layout(context, descriptor_buffer,
                                    mutable_descriptor, descriptor_count,
                                    variable_descriptor_count,
                                    vkd3d_aux_binding),
        create_set_layout(context.device(), sampler_bindings,
                          descriptor_buffer),
        create_set_layout(context.device(), storage_bindings,
                          descriptor_buffer),
    };
}

bool test_mutable_descriptor_set(Context& context, Buffer& output,
                                 const Image& image)
{
    DescriptorObjects objects = create_compute_objects(
        context, create_mutable_test_layouts(context, false),
        descriptor_mutable_comp_spv, sizeof(descriptor_mutable_comp_spv),
        false, sizeof(uint32_t));

    const std::array<VkDescriptorPoolSize, 3> pool_sizes{{
        {VK_DESCRIPTOR_TYPE_MUTABLE_EXT, kMutableDescriptorCount},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
    }};
    std::array<VkMutableDescriptorTypeListEXT, 3> pool_mutable_lists{};
    pool_mutable_lists[0].descriptorTypeCount =
        static_cast<uint32_t>(kMutableTypes.size());
    pool_mutable_lists[0].pDescriptorTypes = kMutableTypes.data();
    VkMutableDescriptorTypeCreateInfoEXT pool_mutable_info{
        VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT};
    pool_mutable_info.mutableDescriptorTypeListCount =
        static_cast<uint32_t>(pool_mutable_lists.size());
    pool_mutable_info.pMutableDescriptorTypeLists =
        pool_mutable_lists.data();

    VkDescriptorPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.pNext = &pool_mutable_info;
    pool_info.maxSets = 3;
    pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();
    check(vkCreateDescriptorPool(context.device(), &pool_info, nullptr,
                                 &objects.descriptor_pool),
          "vkCreateDescriptorPool(mutable)");

    const std::array<uint32_t, 3> variable_counts{{
        kMutableDescriptorCount, 0, 0,
    }};
    VkDescriptorSetVariableDescriptorCountAllocateInfo variable_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
    variable_info.descriptorSetCount =
        static_cast<uint32_t>(variable_counts.size());
    variable_info.pDescriptorCounts = variable_counts.data();
    VkDescriptorSetAllocateInfo allocate_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate_info.pNext = &variable_info;
    allocate_info.descriptorPool = objects.descriptor_pool;
    allocate_info.descriptorSetCount =
        static_cast<uint32_t>(objects.set_layouts.size());
    allocate_info.pSetLayouts = objects.set_layouts.data();
    std::array<VkDescriptorSet, 3> sets{};
    check(vkAllocateDescriptorSets(context.device(), &allocate_info,
                                   sets.data()),
          "vkAllocateDescriptorSets(mutable)");

    VkDescriptorImageInfo image_info{
        VK_NULL_HANDLE, image.view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo sampler_info{
        image.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkDescriptorBufferInfo output_info{
        output.buffer, 0, sizeof(uint32_t)};
    std::array<VkWriteDescriptorSet, 3> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet = sets[0];
    writes[0].dstBinding = 0;
    writes[0].dstArrayElement = kMutableDescriptorIndex;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo = &image_info;
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[1].dstSet = sets[1];
    writes[1].dstBinding = 0;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[1].pImageInfo = &sampler_info;
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[2].dstSet = sets[2];
    writes[2].dstBinding = 0;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &output_info;
    vkUpdateDescriptorSets(context.device(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);

    reset_output(context, output);
    context.submit([&](VkCommandBuffer command_buffer) {
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          objects.pipeline);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                objects.pipeline_layout, 0,
                                static_cast<uint32_t>(sets.size()),
                                sets.data(), 0, nullptr);
        const uint32_t descriptor_index = kMutableDescriptorIndex;
        vkCmdPushConstants(command_buffer, objects.pipeline_layout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(descriptor_index), &descriptor_index);
        vkCmdDispatch(command_buffer, 1, 1, 1);
        add_compute_to_host_barrier(command_buffer, output.buffer);
    });
    return report_result(
        "mutable descriptor-set large sampled-image array",
        read_output(context, output), kImageExpected);
}

size_t mutable_descriptor_stride(
    const VkPhysicalDeviceDescriptorBufferPropertiesEXT& properties)
{
    return static_cast<size_t>(std::max({
        properties.robustUniformBufferDescriptorSize,
        properties.robustStorageBufferDescriptorSize,
        properties.sampledImageDescriptorSize,
        properties.robustUniformTexelBufferDescriptorSize,
        properties.storageImageDescriptorSize,
        properties.robustStorageTexelBufferDescriptorSize,
    }));
}

bool test_sampled_array_descriptor_buffer(
    Context& context, Buffer& output, const Image& image,
    bool mutable_descriptor, uint32_t descriptor_count,
    uint32_t descriptor_index, VkDeviceSize requested_base_offset,
    const uint32_t* shader_code, size_t shader_size,
    const char* indexing_name,
    bool variable_descriptor_count = true,
    bool vkd3d_aux_binding = false)
{
    DescriptorObjects objects = create_compute_objects(
        context, create_mutable_test_layouts(
                     context, true, mutable_descriptor, descriptor_count,
                     variable_descriptor_count, vkd3d_aux_binding),
        shader_code, shader_size,
        true, sizeof(uint32_t));
    const auto& properties = context.descriptor_properties();
    const VkDeviceSize alignment =
        properties.descriptorBufferOffsetAlignment;

    std::array<VkDeviceSize, 3> layout_sizes{};
    std::array<VkDeviceSize, 3> binding_offsets{};
    const uint32_t resource_array_binding = vkd3d_aux_binding ? 1 : 0;
    for (size_t i = 0; i < objects.set_layouts.size(); ++i) {
        context.get_layout_size(context.device(), objects.set_layouts[i],
                                &layout_sizes[i]);
        context.get_binding_offset(context.device(), objects.set_layouts[i],
                                   i == 0 ? resource_array_binding : 0,
                                   &binding_offsets[i]);
    }
    VkDeviceSize aux_binding_offset = 0;
    if (vkd3d_aux_binding) {
        context.get_binding_offset(context.device(), objects.set_layouts[0],
                                   0, &aux_binding_offset);
    }

    const std::array<VkDeviceSize, 3> base_offsets{{
        align_up(requested_base_offset, alignment),
        align_up(128, alignment),
        align_up(64, alignment),
    }};
    const size_t descriptor_stride = mutable_descriptor
        ? mutable_descriptor_stride(properties)
        : properties.sampledImageDescriptorSize;
    const VkDeviceSize resource_payload_offset =
        base_offsets[0] + binding_offsets[0] +
        descriptor_index * descriptor_stride;
    const VkDeviceSize sampler_payload_offset =
        base_offsets[1] + binding_offsets[1];
    const VkDeviceSize storage_payload_offset =
        base_offsets[2] + binding_offsets[2];

    const VkDeviceSize resource_size = align_up(
        std::max(base_offsets[0] + layout_sizes[0],
                 resource_payload_offset +
                     properties.sampledImageDescriptorSize),
        alignment);
    const VkDeviceSize sampler_size = align_up(
        std::max(base_offsets[1] + layout_sizes[1],
                 sampler_payload_offset + properties.samplerDescriptorSize),
        alignment);
    const VkDeviceSize storage_size = align_up(
        std::max(base_offsets[2] + layout_sizes[2],
                 storage_payload_offset +
                     properties.storageBufferDescriptorSize),
        alignment);

    Buffer resource_descriptors = context.create_buffer(
        resource_size,
        VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, true);
    Buffer sampler_descriptors = context.create_buffer(
        sampler_size,
        VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, true);
    Buffer storage_descriptors = context.create_buffer(
        storage_size,
        VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, true);
    Buffer aux_value = context.create_buffer(
        sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, true);
    std::memset(resource_descriptors.mapped, 0,
                static_cast<size_t>(resource_size));
    std::memset(sampler_descriptors.mapped, 0,
                static_cast<size_t>(sampler_size));
    std::memset(storage_descriptors.mapped, 0,
                static_cast<size_t>(storage_size));
    std::memset(aux_value.mapped, 0, sizeof(uint32_t));
    context.flush(aux_value);

    VkDescriptorImageInfo image_info{
        VK_NULL_HANDLE, image.view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorGetInfoEXT sampled_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    sampled_info.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sampled_info.data.pSampledImage = &image_info;
    context.get_descriptor(
        context.device(), &sampled_info,
        properties.sampledImageDescriptorSize,
        static_cast<uint8_t*>(resource_descriptors.mapped) +
            resource_payload_offset);

    if (vkd3d_aux_binding) {
        VkDescriptorAddressInfoEXT aux_address{
            VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
        aux_address.address = aux_value.address;
        aux_address.range = sizeof(uint32_t);
        aux_address.format = VK_FORMAT_UNDEFINED;
        VkDescriptorGetInfoEXT aux_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
        aux_info.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        aux_info.data.pStorageBuffer = &aux_address;
        context.get_descriptor(
            context.device(), &aux_info,
            properties.storageBufferDescriptorSize,
            static_cast<uint8_t*>(resource_descriptors.mapped) +
                base_offsets[0] + aux_binding_offset);
    }

    VkDescriptorGetInfoEXT sampler_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    sampler_info.type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sampler_info.data.pSampler = &image.sampler;
    context.get_descriptor(
        context.device(), &sampler_info,
        properties.samplerDescriptorSize,
        static_cast<uint8_t*>(sampler_descriptors.mapped) +
            sampler_payload_offset);

    VkDescriptorAddressInfoEXT output_address{
        VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
    output_address.address = output.address;
    output_address.range = sizeof(uint32_t);
    output_address.format = VK_FORMAT_UNDEFINED;
    VkDescriptorGetInfoEXT storage_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    storage_info.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    storage_info.data.pStorageBuffer = &output_address;
    context.get_descriptor(
        context.device(), &storage_info,
        properties.storageBufferDescriptorSize,
        static_cast<uint8_t*>(storage_descriptors.mapped) +
            storage_payload_offset);
    context.flush(resource_descriptors);
    context.flush(sampler_descriptors);
    context.flush(storage_descriptors);

    std::cout << (mutable_descriptor ? "Mutable" : "Typed")
              << " descriptor-buffer layout: count="
              << descriptor_count
              << " selected-index=" << descriptor_index
              << " stride=" << descriptor_stride
              << " layout-size=" << layout_sizes[0]
              << " binding-offset=" << binding_offsets[0]
              << " set-base=" << base_offsets[0] << '\n';
    print_payload(mutable_descriptor
                      ? "mutable sampled-image"
                      : "typed sampled-image",
                  resource_descriptors,
                  resource_payload_offset,
                  properties.sampledImageDescriptorSize);

    reset_output(context, output);
    context.submit([&](VkCommandBuffer command_buffer) {
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          objects.pipeline);
        const std::array<VkDescriptorBufferBindingInfoEXT, 3> bindings{{
            {VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT, nullptr,
             resource_descriptors.address,
             VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT},
            {VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT, nullptr,
             sampler_descriptors.address,
             VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT},
            {VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT, nullptr,
             storage_descriptors.address,
             VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT},
        }};
        context.cmd_bind_descriptor_buffers(
            command_buffer, static_cast<uint32_t>(bindings.size()),
            bindings.data());
        const std::array<uint32_t, 3> buffer_indices{{0, 1, 2}};
        context.cmd_set_descriptor_offsets(
            command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            objects.pipeline_layout, 0,
            static_cast<uint32_t>(buffer_indices.size()),
            buffer_indices.data(), base_offsets.data());
        vkCmdPushConstants(command_buffer, objects.pipeline_layout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(descriptor_index), &descriptor_index);
        vkCmdDispatch(command_buffer, 1, 1, 1);
        add_compute_to_host_barrier(command_buffer, output.buffer);
    });
    std::ostringstream name;
    name << indexing_name << ' '
         << (mutable_descriptor ? "mutable" : "typed")
         << " descriptor-buffer array count=" << descriptor_count
         << " index=" << descriptor_index
         << " variable=" << (variable_descriptor_count ? "yes" : "no")
         << " aux-binding=" << (vkd3d_aux_binding ? "yes" : "no")
         << " set-offset=" << base_offsets[0];
    return report_result(name.str().c_str(), read_output(context, output),
                         kImageExpected);
}

bool test_sampler_array_descriptor_buffer(Context& context, Buffer& output,
                                          const Image& image)
{
    constexpr uint32_t descriptor_count = 2048;
    constexpr uint32_t descriptor_index = 1023;
    const std::vector<VkDescriptorSetLayoutBinding> resource_bindings{{
        0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr,
    }};
    const std::vector<VkDescriptorSetLayoutBinding> sampler_bindings{{
        0, VK_DESCRIPTOR_TYPE_SAMPLER, descriptor_count,
        VK_SHADER_STAGE_ALL, nullptr,
    }};
    const std::vector<VkDescriptorSetLayoutBinding> storage_bindings{{
        0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr,
    }};
    std::vector<VkDescriptorSetLayout> layouts{
        create_set_layout(context.device(), resource_bindings, true),
        create_set_layout(context.device(), sampler_bindings, true),
        create_set_layout(context.device(), storage_bindings, true),
    };
    DescriptorObjects objects = create_compute_objects(
        context, std::move(layouts), descriptor_sampler_array_comp_spv,
        sizeof(descriptor_sampler_array_comp_spv), true, sizeof(uint32_t));

    const auto& properties = context.descriptor_properties();
    const VkDeviceSize alignment =
        properties.descriptorBufferOffsetAlignment;
    std::array<VkDeviceSize, 3> layout_sizes{};
    std::array<VkDeviceSize, 3> binding_offsets{};
    for (size_t i = 0; i < objects.set_layouts.size(); ++i) {
        context.get_layout_size(context.device(), objects.set_layouts[i],
                                &layout_sizes[i]);
        context.get_binding_offset(context.device(), objects.set_layouts[i],
                                   0, &binding_offsets[i]);
    }
    const std::array<VkDeviceSize, 3> base_offsets{{
        0, align_up(256, alignment), align_up(64, alignment),
    }};
    const VkDeviceSize resource_payload_offset =
        base_offsets[0] + binding_offsets[0];
    const VkDeviceSize sampler_payload_offset =
        base_offsets[1] + binding_offsets[1] +
        descriptor_index * properties.samplerDescriptorSize;
    const VkDeviceSize storage_payload_offset =
        base_offsets[2] + binding_offsets[2];

    Buffer resource_descriptors = context.create_buffer(
        align_up(std::max(base_offsets[0] + layout_sizes[0],
                          resource_payload_offset +
                              properties.sampledImageDescriptorSize),
                 alignment),
        VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, true);
    Buffer sampler_descriptors = context.create_buffer(
        align_up(std::max(base_offsets[1] + layout_sizes[1],
                          sampler_payload_offset +
                              properties.samplerDescriptorSize),
                 alignment),
        VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, true);
    Buffer storage_descriptors = context.create_buffer(
        align_up(std::max(base_offsets[2] + layout_sizes[2],
                          storage_payload_offset +
                              properties.storageBufferDescriptorSize),
                 alignment),
        VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, true);
    std::memset(resource_descriptors.mapped, 0,
                static_cast<size_t>(resource_descriptors.size));
    std::memset(sampler_descriptors.mapped, 0,
                static_cast<size_t>(sampler_descriptors.size));
    std::memset(storage_descriptors.mapped, 0,
                static_cast<size_t>(storage_descriptors.size));

    VkDescriptorImageInfo image_info{
        VK_NULL_HANDLE, image.view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorGetInfoEXT sampled_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    sampled_info.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sampled_info.data.pSampledImage = &image_info;
    context.get_descriptor(
        context.device(), &sampled_info,
        properties.sampledImageDescriptorSize,
        static_cast<uint8_t*>(resource_descriptors.mapped) +
            resource_payload_offset);

    VkDescriptorGetInfoEXT sampler_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    sampler_info.type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sampler_info.data.pSampler = &image.sampler;
    context.get_descriptor(
        context.device(), &sampler_info,
        properties.samplerDescriptorSize,
        static_cast<uint8_t*>(sampler_descriptors.mapped) +
            sampler_payload_offset);

    VkDescriptorAddressInfoEXT output_address{
        VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
    output_address.address = output.address;
    output_address.range = sizeof(uint32_t);
    output_address.format = VK_FORMAT_UNDEFINED;
    VkDescriptorGetInfoEXT storage_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    storage_info.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    storage_info.data.pStorageBuffer = &output_address;
    context.get_descriptor(
        context.device(), &storage_info,
        properties.storageBufferDescriptorSize,
        static_cast<uint8_t*>(storage_descriptors.mapped) +
            storage_payload_offset);
    context.flush(resource_descriptors);
    context.flush(sampler_descriptors);
    context.flush(storage_descriptors);

    reset_output(context, output);
    context.submit([&](VkCommandBuffer command_buffer) {
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          objects.pipeline);
        const std::array<VkDescriptorBufferBindingInfoEXT, 3> bindings{{
            {VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT, nullptr,
             resource_descriptors.address,
             VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT},
            {VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT, nullptr,
             sampler_descriptors.address,
             VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT},
            {VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT, nullptr,
             storage_descriptors.address,
             VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT},
        }};
        context.cmd_bind_descriptor_buffers(
            command_buffer, static_cast<uint32_t>(bindings.size()),
            bindings.data());
        const std::array<uint32_t, 3> buffer_indices{{0, 1, 2}};
        context.cmd_set_descriptor_offsets(
            command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            objects.pipeline_layout, 0,
            static_cast<uint32_t>(buffer_indices.size()),
            buffer_indices.data(), base_offsets.data());
        vkCmdPushConstants(command_buffer, objects.pipeline_layout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(descriptor_index), &descriptor_index);
        vkCmdDispatch(command_buffer, 1, 1, 1);
        add_compute_to_host_barrier(command_buffer, output.buffer);
    });
    return report_result(
        "fixed-count runtime sampler descriptor-buffer array",
        read_output(context, output), kImageExpected);
}

bool test_graphics_descriptor_buffer(
    Context& context, Buffer& output, const Image& image,
    bool mutable_descriptor)
{
    constexpr uint32_t descriptor_count = 1000000;
    constexpr uint32_t descriptor_index = 524287;
    Image render_target = context.create_render_target();
    const std::vector<VkDescriptorSetLayoutBinding> sampler_bindings{{
        0, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
        VK_SHADER_STAGE_FRAGMENT_BIT, nullptr,
    }};
    std::vector<VkDescriptorSetLayout> layouts{
        create_sampled_array_layout(context, true, mutable_descriptor,
                                    descriptor_count, false, true),
        create_set_layout(context.device(), sampler_bindings, true),
    };
    DescriptorObjects objects = create_graphics_objects(
        context, std::move(layouts), render_target);

    const auto& properties = context.descriptor_properties();
    const VkDeviceSize alignment =
        properties.descriptorBufferOffsetAlignment;
    VkDeviceSize resource_layout_size = 0;
    VkDeviceSize resource_array_offset = 0;
    VkDeviceSize aux_offset = 0;
    VkDeviceSize sampler_layout_size = 0;
    VkDeviceSize sampler_offset = 0;
    context.get_layout_size(context.device(), objects.set_layouts[0],
                            &resource_layout_size);
    context.get_binding_offset(context.device(), objects.set_layouts[0], 0,
                               &aux_offset);
    context.get_binding_offset(context.device(), objects.set_layouts[0], 1,
                               &resource_array_offset);
    context.get_layout_size(context.device(), objects.set_layouts[1],
                            &sampler_layout_size);
    context.get_binding_offset(context.device(), objects.set_layouts[1], 0,
                               &sampler_offset);

    const VkDeviceSize resource_base = align_up(256, alignment);
    const VkDeviceSize sampler_base = align_up(128, alignment);
    const size_t descriptor_stride = mutable_descriptor
        ? mutable_descriptor_stride(properties)
        : properties.sampledImageDescriptorSize;
    const VkDeviceSize resource_payload_offset =
        resource_base + resource_array_offset +
        descriptor_index * descriptor_stride;
    const VkDeviceSize sampler_payload_offset =
        sampler_base + sampler_offset;
    const VkDeviceSize resource_size = align_up(
        std::max(resource_base + resource_layout_size,
                 resource_payload_offset +
                     properties.sampledImageDescriptorSize),
        alignment);
    const VkDeviceSize sampler_size = align_up(
        std::max(sampler_base + sampler_layout_size,
                 sampler_payload_offset +
                     properties.samplerDescriptorSize),
        alignment);

    Buffer resource_descriptors = context.create_buffer(
        resource_size,
        VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, true);
    Buffer sampler_descriptors = context.create_buffer(
        sampler_size,
        VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, true);
    Buffer aux_value = context.create_buffer(
        sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, true);
    std::memset(resource_descriptors.mapped, 0,
                static_cast<size_t>(resource_size));
    std::memset(sampler_descriptors.mapped, 0,
                static_cast<size_t>(sampler_size));
    std::memset(aux_value.mapped, 0, sizeof(uint32_t));

    VkDescriptorAddressInfoEXT aux_address{
        VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
    aux_address.address = aux_value.address;
    aux_address.range = sizeof(uint32_t);
    aux_address.format = VK_FORMAT_UNDEFINED;
    VkDescriptorGetInfoEXT aux_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    aux_info.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    aux_info.data.pStorageBuffer = &aux_address;
    context.get_descriptor(
        context.device(), &aux_info,
        properties.storageBufferDescriptorSize,
        static_cast<uint8_t*>(resource_descriptors.mapped) +
            resource_base + aux_offset);

    VkDescriptorImageInfo image_info{
        VK_NULL_HANDLE, image.view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorGetInfoEXT sampled_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    sampled_info.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sampled_info.data.pSampledImage = &image_info;
    context.get_descriptor(
        context.device(), &sampled_info,
        properties.sampledImageDescriptorSize,
        static_cast<uint8_t*>(resource_descriptors.mapped) +
            resource_payload_offset);

    VkDescriptorGetInfoEXT sampler_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    sampler_info.type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sampler_info.data.pSampler = &image.sampler;
    context.get_descriptor(
        context.device(), &sampler_info,
        properties.samplerDescriptorSize,
        static_cast<uint8_t*>(sampler_descriptors.mapped) +
            sampler_payload_offset);
    context.flush(aux_value);
    context.flush(resource_descriptors);
    context.flush(sampler_descriptors);
    reset_output(context, output);

    context.submit([&](VkCommandBuffer command_buffer) {
        const VkClearValue clear{{{0.0f, 0.0f, 0.0f, 0.0f}}};
        VkRenderPassBeginInfo begin{
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = objects.render_pass;
        begin.framebuffer = objects.framebuffer;
        begin.renderArea.extent = {1, 1};
        begin.clearValueCount = 1;
        begin.pClearValues = &clear;
        vkCmdBeginRenderPass(command_buffer, &begin,
                             VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          objects.pipeline);
        const std::array<VkDescriptorBufferBindingInfoEXT, 2> bindings{{
            {VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT, nullptr,
             resource_descriptors.address,
             VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT},
            {VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT, nullptr,
             sampler_descriptors.address,
             VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT},
        }};
        context.cmd_bind_descriptor_buffers(
            command_buffer, static_cast<uint32_t>(bindings.size()),
            bindings.data());
        const std::array<uint32_t, 2> buffer_indices{{0, 1}};
        const std::array<VkDeviceSize, 2> offsets{{
            resource_base, sampler_base,
        }};
        context.cmd_set_descriptor_offsets(
            command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            objects.pipeline_layout, 0,
            static_cast<uint32_t>(buffer_indices.size()),
            buffer_indices.data(), offsets.data());
        vkCmdPushConstants(command_buffer, objects.pipeline_layout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(descriptor_index), &descriptor_index);
        vkCmdDraw(command_buffer, 3, 1, 0, 0);
        vkCmdEndRenderPass(command_buffer);

        VkImageMemoryBarrier image_barrier{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        image_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        image_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_barrier.image = render_target.image;
        image_barrier.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        image_barrier.subresourceRange.levelCount = 1;
        image_barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(
            command_buffer,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, nullptr, 0, nullptr, 1, &image_barrier);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {1, 1, 1};
        vkCmdCopyImageToBuffer(
            command_buffer, render_target.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.buffer, 1, &copy);

        VkBufferMemoryBarrier output_barrier{
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        output_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        output_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        output_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        output_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        output_barrier.buffer = output.buffer;
        output_barrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command_buffer,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, nullptr, 1, &output_barrier, 0, nullptr);
    });

    return report_result(
        mutable_descriptor
            ? "fragment-stage mutable descriptor-buffer array"
            : "fragment-stage typed descriptor-buffer array",
        read_output(context, output), kImageExpected);
}

bool test_graphics_texel_descriptor_buffer(
    Context& context, Buffer& output, bool mutable_descriptor)
{
    constexpr uint32_t descriptor_count = 1000000;
    constexpr std::array<uint32_t, 2> descriptor_indices{{
        524287, 524288,
    }};
    constexpr float float_value = 0.25f;

    Image render_target = context.create_render_target();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (mutable_descriptor) {
        layout = create_sampled_array_layout(
            context, true, true, descriptor_count, false, false);
    } else {
        const std::vector<VkDescriptorSetLayoutBinding> bindings{{
            0, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, descriptor_count,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            nullptr,
        }};
        layout = create_set_layout(context.device(), bindings, true);
    }
    DescriptorObjects objects = create_graphics_objects(
        context, {layout}, render_target,
        descriptor_texel_graphics_vert_spv,
        sizeof(descriptor_texel_graphics_vert_spv),
        descriptor_texel_graphics_frag_spv,
        sizeof(descriptor_texel_graphics_frag_spv),
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        sizeof(descriptor_indices));

    const auto& properties = context.descriptor_properties();
    const VkDeviceSize alignment =
        properties.descriptorBufferOffsetAlignment;
    VkDeviceSize layout_size = 0;
    VkDeviceSize binding_offset = 0;
    context.get_layout_size(context.device(), objects.set_layouts[0],
                            &layout_size);
    context.get_binding_offset(context.device(), objects.set_layouts[0], 0,
                               &binding_offset);

    const VkDeviceSize base_offset = align_up(256, alignment);
    const VkDeviceSize descriptor_stride = mutable_descriptor
        ? mutable_descriptor_stride(properties)
        : properties.uniformTexelBufferDescriptorSize;
    const VkDeviceSize uint_descriptor_offset =
        base_offset + binding_offset +
        descriptor_indices[0] * descriptor_stride;
    const VkDeviceSize float_descriptor_offset =
        base_offset + binding_offset +
        descriptor_indices[1] * descriptor_stride;
    const VkDeviceSize descriptor_size = mutable_descriptor
        ? properties.robustUniformTexelBufferDescriptorSize
        : properties.uniformTexelBufferDescriptorSize;
    const VkDeviceSize allocation_size = align_up(
        std::max(base_offset + layout_size,
                 float_descriptor_offset + descriptor_size),
        alignment);

    Buffer descriptors = context.create_buffer(
        allocation_size,
        VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, true);
    Buffer texels = context.create_buffer(
        2 * sizeof(uint32_t),
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, true);
    std::memset(descriptors.mapped, 0,
                static_cast<size_t>(descriptors.size));
    std::memcpy(texels.mapped, &kImageExpected, sizeof(kImageExpected));
    std::memcpy(static_cast<uint8_t*>(texels.mapped) + sizeof(uint32_t),
                &float_value, sizeof(float_value));

    VkDescriptorAddressInfoEXT uint_address{
        VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
    uint_address.address = texels.address;
    uint_address.range = sizeof(uint32_t);
    uint_address.format = VK_FORMAT_R32_UINT;
    VkDescriptorGetInfoEXT uint_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    uint_info.type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    uint_info.data.pUniformTexelBuffer = &uint_address;
    context.get_descriptor(
        context.device(), &uint_info, descriptor_size,
        static_cast<uint8_t*>(descriptors.mapped) +
            uint_descriptor_offset);

    VkDescriptorAddressInfoEXT float_address{
        VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
    float_address.address = texels.address + sizeof(uint32_t);
    float_address.range = sizeof(uint32_t);
    float_address.format = VK_FORMAT_R32_SFLOAT;
    VkDescriptorGetInfoEXT float_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    float_info.type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    float_info.data.pUniformTexelBuffer = &float_address;
    context.get_descriptor(
        context.device(), &float_info, descriptor_size,
        static_cast<uint8_t*>(descriptors.mapped) +
            float_descriptor_offset);
    context.flush(texels);
    context.flush(descriptors);
    reset_output(context, output);

    context.submit([&](VkCommandBuffer command_buffer) {
        const VkClearValue clear{{{0.0f, 0.0f, 0.0f, 0.0f}}};
        VkRenderPassBeginInfo begin{
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = objects.render_pass;
        begin.framebuffer = objects.framebuffer;
        begin.renderArea.extent = {1, 1};
        begin.clearValueCount = 1;
        begin.pClearValues = &clear;
        vkCmdBeginRenderPass(command_buffer, &begin,
                             VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          objects.pipeline);
        VkDescriptorBufferBindingInfoEXT binding{
            VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT};
        binding.address = descriptors.address;
        binding.usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;
        context.cmd_bind_descriptor_buffers(command_buffer, 1, &binding);
        const uint32_t buffer_index = 0;
        context.cmd_set_descriptor_offsets(
            command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            objects.pipeline_layout, 0, 1, &buffer_index, &base_offset);
        vkCmdPushConstants(
            command_buffer, objects.pipeline_layout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(descriptor_indices), descriptor_indices.data());
        vkCmdDraw(command_buffer, 3, 1, 0, 0);
        vkCmdEndRenderPass(command_buffer);

        VkImageMemoryBarrier image_barrier{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        image_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        image_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_barrier.image = render_target.image;
        image_barrier.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        image_barrier.subresourceRange.levelCount = 1;
        image_barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(
            command_buffer,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, nullptr, 0, nullptr, 1, &image_barrier);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {1, 1, 1};
        vkCmdCopyImageToBuffer(
            command_buffer, render_target.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.buffer, 1, &copy);

        VkBufferMemoryBarrier output_barrier{
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        output_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        output_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        output_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        output_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        output_barrier.buffer = output.buffer;
        output_barrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command_buffer,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, nullptr, 1, &output_barrier, 0, nullptr);
    });

    return report_result(
        mutable_descriptor
            ? "graphics mutable texel descriptor-buffer aliases"
            : "graphics typed texel descriptor-buffer aliases",
        read_output(context, output), kImageExpected);
}

} // namespace

int main()
{
    try {
        Context context;
        Buffer output = context.create_buffer(
            sizeof(uint32_t),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, true);
        Image image = context.create_test_image();

        bool passed = true;
        passed &= test_storage_descriptor_set(context, output);
        passed &= test_storage_descriptor_buffer(context, output);
        passed &= test_uniform_buffer(context, output, false);
        passed &= test_uniform_buffer(context, output, true);
        passed &= test_descriptor_buffer_push_descriptor(context, output);
        passed &= test_image_descriptor_set(context, output, image);
        passed &= test_image_descriptor_buffer(context, output, image);
        passed &= test_sampler_array_descriptor_buffer(
            context, output, image);
        passed &= test_graphics_descriptor_buffer(
            context, output, image, false);
        passed &= test_graphics_descriptor_buffer(
            context, output, image, true);
        passed &= test_graphics_texel_descriptor_buffer(
            context, output, false);
        passed &= test_graphics_texel_descriptor_buffer(
            context, output, true);
        passed &= test_mutable_descriptor_set(context, output, image);
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, false, 4, 3, 0,
            descriptor_fixed_array_comp_spv,
            sizeof(descriptor_fixed_array_comp_spv),
            "fixed-index", false);
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, false, 4, 3, 0,
            descriptor_fixed_array_comp_spv,
            sizeof(descriptor_fixed_array_comp_spv),
            "fixed-index", true);
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, true, 4, 3, 0,
            descriptor_fixed_array_comp_spv,
            sizeof(descriptor_fixed_array_comp_spv),
            "fixed-index", false);
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, true, 4, 3, 0,
            descriptor_fixed_array_comp_spv,
            sizeof(descriptor_fixed_array_comp_spv),
            "fixed-index", true);
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, false, 1, 0, 0,
            descriptor_uniform_array_comp_spv,
            sizeof(descriptor_uniform_array_comp_spv),
            "uniform-index");
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, false, 1024, 511, 256,
            descriptor_uniform_array_comp_spv,
            sizeof(descriptor_uniform_array_comp_spv),
            "uniform-index");
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, true, 1, 0, 0,
            descriptor_uniform_array_comp_spv,
            sizeof(descriptor_uniform_array_comp_spv),
            "uniform-index");
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, true, 1024, 511, 256,
            descriptor_uniform_array_comp_spv,
            sizeof(descriptor_uniform_array_comp_spv),
            "uniform-index");
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, false, 1, 0, 0,
            descriptor_mutable_comp_spv,
            sizeof(descriptor_mutable_comp_spv),
            "nonuniform-index");
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, false, 1024, 511, 256,
            descriptor_mutable_comp_spv,
            sizeof(descriptor_mutable_comp_spv),
            "nonuniform-index");
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, true, 1, 0, 0,
            descriptor_mutable_comp_spv,
            sizeof(descriptor_mutable_comp_spv),
            "nonuniform-index");
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, true, 1024, 511, 256,
            descriptor_mutable_comp_spv,
            sizeof(descriptor_mutable_comp_spv),
            "nonuniform-index");
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, false, 1, 0, 0,
            descriptor_uniform_array_comp_spv,
            sizeof(descriptor_uniform_array_comp_spv),
            "uniform-runtime-index", false);
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, false, 1024, 511, 256,
            descriptor_uniform_array_comp_spv,
            sizeof(descriptor_uniform_array_comp_spv),
            "uniform-runtime-index", false);
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, true, 1, 0, 0,
            descriptor_uniform_array_comp_spv,
            sizeof(descriptor_uniform_array_comp_spv),
            "uniform-runtime-index", false);
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, true, 1024, 511, 256,
            descriptor_uniform_array_comp_spv,
            sizeof(descriptor_uniform_array_comp_spv),
            "uniform-runtime-index", false);
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, false, 1, 0, 0,
            descriptor_mutable_comp_spv,
            sizeof(descriptor_mutable_comp_spv),
            "nonuniform-runtime-index", false);
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, false, 1024, 511, 256,
            descriptor_mutable_comp_spv,
            sizeof(descriptor_mutable_comp_spv),
            "nonuniform-runtime-index", false);
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, true, 1, 0, 0,
            descriptor_mutable_comp_spv,
            sizeof(descriptor_mutable_comp_spv),
            "nonuniform-runtime-index", false);
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, true, 1024, 511, 256,
            descriptor_mutable_comp_spv,
            sizeof(descriptor_mutable_comp_spv),
            "nonuniform-runtime-index", false);
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, false, 1000000, 524287, 256,
            descriptor_mutable_comp_spv,
            sizeof(descriptor_mutable_comp_spv),
            "vkd3d-sized-nonuniform-runtime-index", false);
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, true, 1000000, 524287, 256,
            descriptor_mutable_comp_spv,
            sizeof(descriptor_mutable_comp_spv),
            "vkd3d-sized-nonuniform-runtime-index", false);
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, true, 1024, 511, 256,
            descriptor_vkd3d_array_comp_spv,
            sizeof(descriptor_vkd3d_array_comp_spv),
            "vkd3d-layout-nonuniform-runtime-index", false, true);
        passed &= test_sampled_array_descriptor_buffer(
            context, output, image, true, 1000000, 524287, 256,
            descriptor_vkd3d_array_comp_spv,
            sizeof(descriptor_vkd3d_array_comp_spv),
            "vkd3d-layout-sized-nonuniform-runtime-index", false, true);

        std::cout << (passed ? "OVERALL: PASS" : "OVERALL: FAIL") << '\n';
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "FATAL: " << error.what() << '\n';
        return 2;
    }
}
