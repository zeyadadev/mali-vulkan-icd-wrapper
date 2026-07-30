#include "compatibility/compatibility_manager.hpp"
#include "compatibility/device_emulation.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

using namespace mali_wrapper::compatibility;

namespace {

void test_profile_detection()
{
    VkApplicationInfo info{};
    info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    info.pEngineName = "vkd3d";
    assert(detect_profile(&info) == Profile::Vkd3d);
    info.pEngineName = "VKD3D";
    assert(detect_profile(&info) == Profile::Vkd3d);
    info.pEngineName = "DXVK";
    assert(detect_profile(&info) == Profile::Native);
    info.pEngineName = "not-vkd3d";
    assert(detect_profile(&info) == Profile::Native);
}

void test_feature_policy()
{
    VkPhysicalDeviceFeatures native{};
    native.textureCompressionBC = VK_TRUE;
    overlay_features({Profile::Native, true}, &native);
    assert(native.pipelineStatisticsQuery == VK_FALSE);
    assert(native.textureCompressionBC == VK_TRUE);

    VkPhysicalDeviceFeatures safe{};
    safe.textureCompressionBC = VK_TRUE;
    overlay_features({Profile::Vkd3d, false}, &safe);
    assert(safe.pipelineStatisticsQuery == VK_TRUE);
    assert(safe.sparseBinding == VK_FALSE);
    assert(safe.textureCompressionBC == VK_TRUE);

    VkPhysicalDeviceFeatures unsafe{};
    overlay_features({Profile::Vkd3d, true}, &unsafe);
    assert(unsafe.pipelineStatisticsQuery == VK_TRUE);
    assert(unsafe.sparseBinding == VK_TRUE);
    assert(unsafe.sparseResidencyImage2D == VK_TRUE);
    assert(unsafe.sparseResidencyImage3D == VK_FALSE);
}

void test_extension_alias()
{
    VkExtensionProperties khr{};
    std::strncpy(khr.extensionName, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME,
                 VK_MAX_EXTENSION_NAME_SIZE - 1);
    khr.specVersion = 1;
    std::vector<VkExtensionProperties> extensions{khr};
    overlay_device_extensions({Profile::Vkd3d, false}, &extensions);
    assert(extensions.size() == 2);
    assert(std::strcmp(extensions[1].extensionName,
                       VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) == 0);
    assert(extensions[1].specVersion == 3);
    overlay_device_extensions({Profile::Vkd3d, false}, &extensions);
    assert(extensions.size() == 2);
}

void test_device_create_transform()
{
    VkPhysicalDeviceRobustness2FeaturesEXT robustness{};
    robustness.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
    robustness.robustBufferAccess2 = VK_TRUE;
    robustness.robustImageAccess2 = VK_TRUE;
    robustness.nullDescriptor = VK_TRUE;

    VkPhysicalDeviceFeatures features{};
    features.pipelineStatisticsQuery = VK_TRUE;
    features.sparseBinding = VK_TRUE;

    const char* names[] = {VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME,
                           VK_EXT_ROBUSTNESS_2_EXTENSION_NAME};
    VkDeviceCreateInfo source{};
    source.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    source.pNext = &robustness;
    source.pEnabledFeatures = &features;
    source.enabledExtensionCount = 2;
    source.ppEnabledExtensionNames = names;

    DeviceCreateTransform transform;
    assert(transform.prepare({Profile::Vkd3d, true}, &source, names, 2));
    const VkDeviceCreateInfo* result = transform.get();
    assert(result->pEnabledFeatures->pipelineStatisticsQuery == VK_FALSE);
    assert(result->pEnabledFeatures->sparseBinding == VK_FALSE);
    assert(std::strcmp(result->ppEnabledExtensionNames[0],
                       VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) == 0);

    const auto* copied = static_cast<const VkPhysicalDeviceRobustness2FeaturesEXT*>(result->pNext);
    assert(copied != &robustness);
    assert(copied->robustBufferAccess2 == VK_FALSE);
    assert(copied->robustImageAccess2 == VK_FALSE);
    assert(copied->nullDescriptor == VK_TRUE);
    assert(robustness.robustBufferAccess2 == VK_TRUE);
    assert(features.sparseBinding == VK_TRUE);
}

void test_device_create_features2_transform()
{
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.features.pipelineStatisticsQuery = VK_TRUE;
    features2.features.vertexPipelineStoresAndAtomics = VK_TRUE;
    features2.features.sparseBinding = VK_TRUE;
    features2.features.sparseResidencyBuffer = VK_TRUE;
    features2.features.sparseResidencyImage2D = VK_TRUE;
    features2.features.sparseResidencyAliased = VK_TRUE;
    features2.features.shaderResourceResidency = VK_TRUE;
    features2.features.shaderResourceMinLod = VK_TRUE;

    VkDeviceCreateInfo source{};
    source.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    source.pNext = &features2;

    DeviceCreateTransform transform;
    assert(transform.prepare({Profile::Vkd3d, true}, &source, nullptr, 0));

    const auto* copied =
        static_cast<const VkPhysicalDeviceFeatures2*>(transform.get()->pNext);
    assert(copied != &features2);
    assert(copied->features.pipelineStatisticsQuery == VK_FALSE);
    assert(copied->features.vertexPipelineStoresAndAtomics == VK_FALSE);
    assert(copied->features.sparseBinding == VK_FALSE);
    assert(copied->features.sparseResidencyBuffer == VK_FALSE);
    assert(copied->features.sparseResidencyImage2D == VK_FALSE);
    assert(copied->features.sparseResidencyAliased == VK_FALSE);
    assert(copied->features.shaderResourceResidency == VK_FALSE);
    assert(copied->features.shaderResourceMinLod == VK_FALSE);

    assert(features2.features.pipelineStatisticsQuery == VK_TRUE);
    assert(features2.features.sparseBinding == VK_TRUE);
}

PFN_vkVoidFunction VKAPI_CALL null_gdpa(VkDevice, const char*)
{
    return nullptr;
}

template <typename Handle>
Handle test_handle(uintptr_t value)
{
    if constexpr (std::is_pointer_v<Handle>) {
        return reinterpret_cast<Handle>(value);
    } else {
        return static_cast<Handle>(value);
    }
}

VkBufferCreateFlags observed_buffer_flags = 0;
bool observed_buffer_bind = false;
bool observed_buffer_free = false;
bool observed_sparse_submit = false;
uint64_t expected_sparse_wait_value = 0;
uint64_t expected_sparse_signal_value = 0;

VkResult VKAPI_CALL mock_create_buffer(
    VkDevice,
    const VkBufferCreateInfo* info,
    const VkAllocationCallbacks*,
    VkBuffer* buffer)
{
    observed_buffer_flags = info->flags;
    *buffer = test_handle<VkBuffer>(0x5000);
    return VK_SUCCESS;
}

void VKAPI_CALL mock_destroy_buffer(
    VkDevice, VkBuffer, const VkAllocationCallbacks*)
{
}

void VKAPI_CALL mock_get_buffer_memory_requirements(
    VkDevice, VkBuffer, VkMemoryRequirements* requirements)
{
    requirements->size = 4096;
    requirements->alignment = 256;
    requirements->memoryTypeBits = 1;
}

VkResult VKAPI_CALL mock_allocate_memory(
    VkDevice,
    const VkMemoryAllocateInfo* info,
    const VkAllocationCallbacks*,
    VkDeviceMemory* memory)
{
    assert(info->allocationSize == 4096);
    *memory = test_handle<VkDeviceMemory>(0x6000);
    return VK_SUCCESS;
}

void VKAPI_CALL mock_free_memory(
    VkDevice, VkDeviceMemory, const VkAllocationCallbacks*)
{
    observed_buffer_free = true;
}

VkResult VKAPI_CALL mock_bind_buffer_memory(
    VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize)
{
    observed_buffer_bind = true;
    return VK_SUCCESS;
}

VkResult VKAPI_CALL mock_queue_submit(
    VkQueue, uint32_t submit_count, const VkSubmitInfo* submits, VkFence)
{
    assert(submit_count == 1);
    assert(submits[0].commandBufferCount == 0);
    const auto* timeline =
        static_cast<const VkTimelineSemaphoreSubmitInfo*>(submits[0].pNext);
    assert(timeline != nullptr);
    assert(timeline->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
    assert(timeline->pNext == nullptr);
    assert(timeline->waitSemaphoreValueCount == 1);
    assert(timeline->signalSemaphoreValueCount == 1);
    assert(timeline->pWaitSemaphoreValues[0] == expected_sparse_wait_value);
    assert(timeline->pSignalSemaphoreValues[0] == expected_sparse_signal_value);
    observed_sparse_submit = true;
    return VK_SUCCESS;
}

PFN_vkVoidFunction VKAPI_CALL sparse_mock_gdpa(VkDevice, const char* name)
{
#define MOCK_PROC(vk_name, function) \
    if (std::strcmp(name, #vk_name) == 0) \
        return reinterpret_cast<PFN_vkVoidFunction>(function)
    MOCK_PROC(vkCreateBuffer, mock_create_buffer);
    MOCK_PROC(vkDestroyBuffer, mock_destroy_buffer);
    MOCK_PROC(vkGetBufferMemoryRequirements, mock_get_buffer_memory_requirements);
    MOCK_PROC(vkAllocateMemory, mock_allocate_memory);
    MOCK_PROC(vkFreeMemory, mock_free_memory);
    MOCK_PROC(vkBindBufferMemory, mock_bind_buffer_memory);
    MOCK_PROC(vkQueueSubmit, mock_queue_submit);
#undef MOCK_PROC
    return nullptr;
}

void test_bounded_query_and_divisor_emulation()
{
    const auto device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(0x1234));
    VkPhysicalDeviceMemoryProperties memory{};
    device_emulation::register_device(
        device, VK_NULL_HANDLE, {Profile::Vkd3d, true}, null_gdpa, memory);

    auto create_query_pool = reinterpret_cast<PFN_vkCreateQueryPool>(
        device_emulation::get_device_hook(device, "vkCreateQueryPool"));
    auto get_query_results = reinterpret_cast<PFN_vkGetQueryPoolResults>(
        device_emulation::get_device_hook(device, "vkGetQueryPoolResults"));
    auto destroy_query_pool = reinterpret_cast<PFN_vkDestroyQueryPool>(
        device_emulation::get_device_hook(device, "vkDestroyQueryPool"));
    auto reset_query_pool_ext = reinterpret_cast<PFN_vkResetQueryPoolEXT>(
        device_emulation::get_device_hook(device, "vkResetQueryPoolEXT"));
    assert(create_query_pool != nullptr);
    assert(get_query_results != nullptr);
    assert(destroy_query_pool != nullptr);
    assert(reset_query_pool_ext != nullptr);

    VkQueryPoolCreateInfo query_info{};
    query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    query_info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
    query_info.queryCount = 2;
    query_info.pipelineStatistics =
        VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
        VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
    VkQueryPool query_pool = VK_NULL_HANDLE;
    assert(create_query_pool(device, &query_info, nullptr, &query_pool) == VK_SUCCESS);

    uint64_t results[6] = {};
    assert(get_query_results(
               device, query_pool, 0, 2, sizeof(results), results,
               3 * sizeof(uint64_t),
               VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) ==
           VK_SUCCESS);
    assert(results[0] == 0 && results[1] == 0 && results[2] == 1);
    assert(results[3] == 0 && results[4] == 0 && results[5] == 1);
    destroy_query_pool(device, query_pool, nullptr);

    VkVertexInputBindingDivisorDescriptionKHR description{};
    description.binding = 0;
    description.divisor = 0;
    VkPipelineVertexInputDivisorStateCreateInfoKHR divisor{};
    divisor.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR;
    divisor.vertexBindingDivisorCount = 1;
    divisor.pVertexBindingDivisors = &description;
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.pNext = &divisor;
    VkGraphicsPipelineCreateInfo pipeline{};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.pVertexInputState = &vertex_input;

    device_emulation::GraphicsPipelineTransform transform;
    transform.prepare(device, 1, &pipeline);
    const auto* transformed_divisor =
        static_cast<const VkPipelineVertexInputDivisorStateCreateInfoKHR*>(
            transform.get()[0].pVertexInputState->pNext);
    assert(transformed_divisor->pVertexBindingDivisors[0].divisor ==
           std::numeric_limits<uint32_t>::max());
    assert(description.divisor == 0);

    device_emulation::unregister_device(device);
}

void test_dense_sparse_capability_scope()
{
    setenv("MALI_WRAPPER_UNSAFE_SPOOF", "1", 1);
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pEngineName = "vkd3d";
    const auto instance = reinterpret_cast<VkInstance>(static_cast<uintptr_t>(0x2000));
    const auto physical =
        reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(0x3000));
    register_instance(instance, &app);
    associate_physical_device(instance, physical);

    uint32_t count = 0;
    device_emulation::get_sparse_image_format_properties(
        physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_TILING_OPTIMAL, &count, nullptr);
    assert(count == 1);
    VkSparseImageFormatProperties properties{};
    device_emulation::get_sparse_image_format_properties(
        physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_TILING_OPTIMAL, &count, &properties);
    assert(count == 1);
    assert(properties.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);

    count = 1;
    device_emulation::get_sparse_image_format_properties(
        physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_3D,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_TILING_OPTIMAL, &count, &properties);
    assert(count == 0);

    unregister_instance(instance);
    unsetenv("MALI_WRAPPER_UNSAFE_SPOOF");
}

void test_dense_sparse_buffer_and_queue_path()
{
    observed_buffer_flags = 0;
    observed_buffer_bind = false;
    observed_buffer_free = false;
    observed_sparse_submit = false;

    const auto device = test_handle<VkDevice>(0x4000);
    const auto queue = test_handle<VkQueue>(0x4100);
    VkPhysicalDeviceMemoryProperties memory{};
    memory.memoryTypeCount = 1;
    memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    device_emulation::register_device(
        device, VK_NULL_HANDLE, {Profile::Vkd3d, true}, sparse_mock_gdpa, memory);

    auto create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(
        device_emulation::get_device_hook(device, "vkCreateBuffer"));
    auto destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(
        device_emulation::get_device_hook(device, "vkDestroyBuffer"));
    assert(create_buffer != nullptr && destroy_buffer != nullptr);

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT |
                 VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
    info.size = 4096;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    assert(create_buffer(device, &info, nullptr, &buffer) == VK_SUCCESS);
    assert((observed_buffer_flags & (VK_BUFFER_CREATE_SPARSE_BINDING_BIT |
                                     VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT)) == 0);
    assert(observed_buffer_bind);
    destroy_buffer(device, buffer, nullptr);
    assert(observed_buffer_free);

    device_emulation::associate_queue(device, queue);
    auto queue_bind_sparse = reinterpret_cast<PFN_vkQueueBindSparse>(
        device_emulation::get_device_hook(device, "vkQueueBindSparse"));
    assert(queue_bind_sparse != nullptr);
    assert(device_emulation::get_device_hook(
               device, "vkGetDeviceImageSparseMemoryRequirementsKHR") != nullptr);
    VkBindSparseInfo bind{};
    bind.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
    const VkSemaphore wait_semaphore = test_handle<VkSemaphore>(0x4200);
    const VkSemaphore signal_semaphore = test_handle<VkSemaphore>(0x4300);
    bind.waitSemaphoreCount = 1;
    bind.pWaitSemaphores = &wait_semaphore;
    bind.signalSemaphoreCount = 1;
    bind.pSignalSemaphores = &signal_semaphore;

    expected_sparse_wait_value = 7;
    expected_sparse_signal_value = 11;
    VkTimelineSemaphoreSubmitInfo timeline{};
    timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline.waitSemaphoreValueCount = 1;
    timeline.pWaitSemaphoreValues = &expected_sparse_wait_value;
    timeline.signalSemaphoreValueCount = 1;
    timeline.pSignalSemaphoreValues = &expected_sparse_signal_value;

    VkDeviceGroupBindSparseInfo group{};
    group.sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_BIND_SPARSE_INFO;
    group.pNext = &timeline;
    bind.pNext = &group;
    assert(queue_bind_sparse(queue, 1, &bind, VK_NULL_HANDLE) == VK_SUCCESS);
    assert(observed_sparse_submit);

    device_emulation::unregister_device(device);
}

} // namespace

int main()
{
    test_profile_detection();
    test_feature_policy();
    test_extension_alias();
    test_device_create_transform();
    test_device_create_features2_transform();
    test_bounded_query_and_divisor_emulation();
    test_dense_sparse_capability_scope();
    test_dense_sparse_buffer_and_queue_path();
    return 0;
}
