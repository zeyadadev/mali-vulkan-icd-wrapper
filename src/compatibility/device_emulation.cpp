#include "compatibility/device_emulation.hpp"

#include "utils/logging.hpp"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace mali_wrapper::compatibility::device_emulation {
namespace {

struct DenseAllocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

struct DenseImage {
    DenseAllocation allocation;
    VkImageCreateInfo create_info{};
};

struct DeviceState {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    ProfileSelection selection{};
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkDeviceSize dense_budget = 8ull * 1024ull * 1024ull * 1024ull;
    VkDeviceSize dense_used = 0;
    std::unordered_map<VkBuffer, DenseAllocation> dense_buffers;
    std::unordered_map<VkImage, DenseImage> dense_images;
};

struct FakeQueryPool {
    VkDevice device = VK_NULL_HANDLE;
    uint32_t query_count = 0;
    VkQueryPipelineStatisticFlags statistics = 0;
    PFN_vkCmdUpdateBuffer cmd_update_buffer = nullptr;
};

std::mutex state_mutex;
std::unordered_map<VkDevice, DeviceState> devices;
std::unordered_map<VkQueue, VkDevice> queue_devices;
std::unordered_map<VkQueryPool, VkDevice> real_query_pool_devices;
std::unordered_map<VkQueryPool, std::unique_ptr<FakeQueryPool>> fake_query_pools;
std::atomic<bool> logged_sparse_approximation{false};

template <typename Handle>
Handle handle_from_pointer(void* pointer)
{
    if constexpr (std::is_pointer_v<Handle>) {
        return reinterpret_cast<Handle>(pointer);
    } else {
        return static_cast<Handle>(reinterpret_cast<uintptr_t>(pointer));
    }
}

template <typename T>
T raw_proc(const DeviceState& state, const char* name)
{
    return state.gdpa != nullptr
        ? reinterpret_cast<T>(state.gdpa(state.device, name))
        : nullptr;
}

bool get_device_state(VkDevice device, DeviceState* out)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    auto it = devices.find(device);
    if (it == devices.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

bool get_query_pool_device(VkQueryPool pool, VkDevice* device)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    auto fake = fake_query_pools.find(pool);
    if (fake != fake_query_pools.end()) {
        *device = fake->second->device;
        return true;
    }
    auto real = real_query_pool_devices.find(pool);
    if (real != real_query_pool_devices.end()) {
        *device = real->second;
        return true;
    }
    return false;
}

bool profile_active(const DeviceState& state)
{
    return state.selection.profile == Profile::Vkd3d;
}

VkDeviceSize parse_budget()
{
    constexpr VkDeviceSize fallback = 8ull * 1024ull * 1024ull * 1024ull;
    const char* value = std::getenv("MALI_WRAPPER_SPARSE_COMMIT_BUDGET");
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long long bytes = std::strtoull(value, &end, 10);
    if (end == value || *end != '\0') {
        LOG_WARN("Ignoring invalid MALI_WRAPPER_SPARSE_COMMIT_BUDGET value");
        return fallback;
    }
    return static_cast<VkDeviceSize>(bytes);
}

bool reserve_dense_budget(VkDevice device, VkDeviceSize size)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    auto it = devices.find(device);
    if (it == devices.end() || it->second.dense_used > it->second.dense_budget ||
        size > it->second.dense_budget - it->second.dense_used) {
        return false;
    }
    it->second.dense_used += size;
    return true;
}

void release_dense_budget(VkDevice device, VkDeviceSize size)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    auto it = devices.find(device);
    if (it != devices.end()) {
        it->second.dense_used =
            size > it->second.dense_used ? 0 : it->second.dense_used - size;
    }
}

uint32_t choose_memory_type(const DeviceState& state, uint32_t type_bits)
{
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < state.memory_properties.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) == 0) {
            continue;
        }
        if (fallback == UINT32_MAX) {
            fallback = i;
        }
        if (state.memory_properties.memoryTypes[i].propertyFlags &
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            return i;
        }
    }
    return fallback;
}

VkImageAspectFlags format_aspects(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

VkSparseImageFormatProperties dense_sparse_format(VkFormat format)
{
    VkSparseImageFormatProperties properties{};
    properties.aspectMask = format_aspects(format);
    properties.imageGranularity = {64, 64, 1};
    properties.flags = VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT;
    return properties;
}

bool is_sparse_buffer(VkBufferCreateFlags flags)
{
    return (flags & (VK_BUFFER_CREATE_SPARSE_BINDING_BIT |
                     VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT |
                     VK_BUFFER_CREATE_SPARSE_ALIASED_BIT)) != 0;
}

bool is_sparse_image(VkImageCreateFlags flags)
{
    return (flags & (VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                     VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                     VK_IMAGE_CREATE_SPARSE_ALIASED_BIT)) != 0;
}

VkBufferCreateFlags strip_sparse_buffer(VkBufferCreateFlags flags)
{
    return flags & ~(VK_BUFFER_CREATE_SPARSE_BINDING_BIT |
                     VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT |
                     VK_BUFFER_CREATE_SPARSE_ALIASED_BIT);
}

VkImageCreateFlags strip_sparse_image(VkImageCreateFlags flags)
{
    return flags & ~(VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                     VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                     VK_IMAGE_CREATE_SPARSE_ALIASED_BIT);
}

VkResult allocate_and_bind_buffer(const DeviceState& state,
                                  VkBuffer buffer,
                                  VkBufferUsageFlags usage,
                                  const VkAllocationCallbacks* allocator,
                                  DenseAllocation* allocation)
{
    auto get_requirements = raw_proc<PFN_vkGetBufferMemoryRequirements>(
        state, "vkGetBufferMemoryRequirements");
    auto allocate = raw_proc<PFN_vkAllocateMemory>(state, "vkAllocateMemory");
    auto bind = raw_proc<PFN_vkBindBufferMemory>(state, "vkBindBufferMemory");
    auto free_memory = raw_proc<PFN_vkFreeMemory>(state, "vkFreeMemory");
    if (get_requirements == nullptr || allocate == nullptr || bind == nullptr ||
        free_memory == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkMemoryRequirements requirements{};
    get_requirements(state.device, buffer, &requirements);
    const uint32_t memory_type = choose_memory_type(state, requirements.memoryTypeBits);
    if (memory_type == UINT32_MAX || !reserve_dense_budget(state.device, requirements.size)) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = memory_type;
    VkMemoryAllocateFlagsInfo flags_info{};
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        flags_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        allocate_info.pNext = &flags_info;
    }
    VkResult result = allocate(state.device, &allocate_info, allocator, &allocation->memory);
    if (result != VK_SUCCESS) {
        release_dense_budget(state.device, requirements.size);
        return result;
    }
    result = bind(state.device, buffer, allocation->memory, 0);
    if (result != VK_SUCCESS) {
        free_memory(state.device, allocation->memory, allocator);
        allocation->memory = VK_NULL_HANDLE;
        release_dense_budget(state.device, requirements.size);
        return result;
    }
    allocation->size = requirements.size;
    return VK_SUCCESS;
}

VkResult allocate_and_bind_image(const DeviceState& state,
                                 VkImage image,
                                 const VkAllocationCallbacks* allocator,
                                 DenseAllocation* allocation)
{
    auto get_requirements = raw_proc<PFN_vkGetImageMemoryRequirements>(
        state, "vkGetImageMemoryRequirements");
    auto allocate = raw_proc<PFN_vkAllocateMemory>(state, "vkAllocateMemory");
    auto bind = raw_proc<PFN_vkBindImageMemory>(state, "vkBindImageMemory");
    auto free_memory = raw_proc<PFN_vkFreeMemory>(state, "vkFreeMemory");
    if (get_requirements == nullptr || allocate == nullptr || bind == nullptr ||
        free_memory == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkMemoryRequirements requirements{};
    get_requirements(state.device, image, &requirements);
    const uint32_t memory_type = choose_memory_type(state, requirements.memoryTypeBits);
    if (memory_type == UINT32_MAX || !reserve_dense_budget(state.device, requirements.size)) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = memory_type;
    VkResult result = allocate(state.device, &allocate_info, allocator, &allocation->memory);
    if (result != VK_SUCCESS) {
        release_dense_budget(state.device, requirements.size);
        return result;
    }
    result = bind(state.device, image, allocation->memory, 0);
    if (result != VK_SUCCESS) {
        free_memory(state.device, allocation->memory, allocator);
        allocation->memory = VK_NULL_HANDLE;
        release_dense_budget(state.device, requirements.size);
        return result;
    }
    allocation->size = requirements.size;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL compat_vkCreateQueryPool(
    VkDevice device,
    const VkQueryPoolCreateInfo* create_info,
    const VkAllocationCallbacks* allocator,
    VkQueryPool* query_pool)
{
    DeviceState state;
    if (!get_device_state(device, &state)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!profile_active(state) || create_info == nullptr || query_pool == nullptr ||
        create_info->queryType != VK_QUERY_TYPE_PIPELINE_STATISTICS) {
        auto raw = raw_proc<PFN_vkCreateQueryPool>(state, "vkCreateQueryPool");
        const VkResult result = raw != nullptr
            ? raw(device, create_info, allocator, query_pool)
            : VK_ERROR_INITIALIZATION_FAILED;
        if (result == VK_SUCCESS && query_pool != nullptr) {
            std::lock_guard<std::mutex> lock(state_mutex);
            real_query_pool_devices[*query_pool] = device;
        }
        return result;
    }

    auto pool = std::make_unique<FakeQueryPool>();
    pool->device = device;
    pool->query_count = create_info->queryCount;
    pool->statistics = create_info->pipelineStatistics;
    pool->cmd_update_buffer = raw_proc<PFN_vkCmdUpdateBuffer>(state, "vkCmdUpdateBuffer");
    *query_pool = handle_from_pointer<VkQueryPool>(pool.get());
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        fake_query_pools.emplace(*query_pool, std::move(pool));
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL compat_vkDestroyQueryPool(
    VkDevice device, VkQueryPool query_pool, const VkAllocationCallbacks* allocator)
{
    DeviceState state;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (fake_query_pools.erase(query_pool) != 0) {
            return;
        }
        real_query_pool_devices.erase(query_pool);
    }
    if (get_device_state(device, &state)) {
        if (auto raw = raw_proc<PFN_vkDestroyQueryPool>(state, "vkDestroyQueryPool")) {
            raw(device, query_pool, allocator);
        }
    }
}

bool get_fake_query_pool(VkQueryPool query_pool, FakeQueryPool* out)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    auto it = fake_query_pools.find(query_pool);
    if (it == fake_query_pools.end()) {
        return false;
    }
    if (out != nullptr) {
        *out = *it->second;
    }
    return true;
}

VKAPI_ATTR VkResult VKAPI_CALL compat_vkGetQueryPoolResults(
    VkDevice device,
    VkQueryPool query_pool,
    uint32_t first_query,
    uint32_t query_count,
    size_t data_size,
    void* data,
    VkDeviceSize stride,
    VkQueryResultFlags flags)
{
    FakeQueryPool pool;
    if (!get_fake_query_pool(query_pool, &pool)) {
        DeviceState state;
        if (!get_device_state(device, &state)) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        auto raw = raw_proc<PFN_vkGetQueryPoolResults>(state, "vkGetQueryPoolResults");
        return raw != nullptr
            ? raw(device, query_pool, first_query, query_count, data_size, data, stride, flags)
            : VK_ERROR_INITIALIZATION_FAILED;
    }
    if (first_query > pool.query_count || query_count > pool.query_count - first_query) {
        return VK_ERROR_UNKNOWN;
    }

    const uint32_t value_count =
        static_cast<uint32_t>(std::bitset<32>(pool.statistics).count());
    const bool use_64 = (flags & VK_QUERY_RESULT_64_BIT) != 0;
    const bool availability = (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0;
    const size_t scalar_size = use_64 ? sizeof(uint64_t) : sizeof(uint32_t);
    const size_t required_per_query = (value_count + (availability ? 1u : 0u)) * scalar_size;
    if (query_count != 0 &&
        (data == nullptr || stride < required_per_query ||
         data_size < required_per_query + static_cast<size_t>(query_count - 1) * stride)) {
        return VK_ERROR_UNKNOWN;
    }

    auto* bytes = static_cast<uint8_t*>(data);
    for (uint32_t query = 0; query < query_count; ++query) {
        std::memset(bytes + static_cast<size_t>(query) * stride, 0, required_per_query);
        if (availability) {
            auto* availability_ptr =
                bytes + static_cast<size_t>(query) * stride + value_count * scalar_size;
            if (use_64) {
                uint64_t one = 1;
                std::memcpy(availability_ptr, &one, sizeof(one));
            } else {
                uint32_t one = 1;
                std::memcpy(availability_ptr, &one, sizeof(one));
            }
        }
    }
    return VK_SUCCESS;
}

template <typename Fn, typename... Args>
void forward_query_command(VkQueryPool query_pool, const char* name, Args... args)
{
    VkDevice device = VK_NULL_HANDLE;
    DeviceState state;
    if (get_query_pool_device(query_pool, &device) && get_device_state(device, &state)) {
        if (auto raw = raw_proc<Fn>(state, name)) {
            raw(args...);
        }
    }
}

VKAPI_ATTR void VKAPI_CALL compat_vkCmdBeginQuery(
    VkCommandBuffer command_buffer,
    VkQueryPool query_pool,
    uint32_t query,
    VkQueryControlFlags flags)
{
    if (get_fake_query_pool(query_pool, nullptr)) {
        return;
    }
    forward_query_command<PFN_vkCmdBeginQuery>(
        query_pool, "vkCmdBeginQuery", command_buffer, query_pool, query, flags);
}

VKAPI_ATTR void VKAPI_CALL compat_vkCmdEndQuery(
    VkCommandBuffer command_buffer, VkQueryPool query_pool, uint32_t query)
{
    if (get_fake_query_pool(query_pool, nullptr)) {
        return;
    }
    forward_query_command<PFN_vkCmdEndQuery>(
        query_pool, "vkCmdEndQuery", command_buffer, query_pool, query);
}

VKAPI_ATTR void VKAPI_CALL compat_vkCmdResetQueryPool(
    VkCommandBuffer command_buffer,
    VkQueryPool query_pool,
    uint32_t first_query,
    uint32_t query_count)
{
    if (get_fake_query_pool(query_pool, nullptr)) {
        return;
    }
    forward_query_command<PFN_vkCmdResetQueryPool>(
        query_pool, "vkCmdResetQueryPool",
        command_buffer, query_pool, first_query, query_count);
}

VKAPI_ATTR void VKAPI_CALL compat_vkResetQueryPool(
    VkDevice device, VkQueryPool query_pool, uint32_t first_query, uint32_t query_count)
{
    if (get_fake_query_pool(query_pool, nullptr)) {
        return;
    }
    DeviceState state;
    if (get_device_state(device, &state)) {
        if (auto raw = raw_proc<PFN_vkResetQueryPool>(state, "vkResetQueryPool")) {
            raw(device, query_pool, first_query, query_count);
        }
    }
}

VKAPI_ATTR void VKAPI_CALL compat_vkCmdCopyQueryPoolResults(
    VkCommandBuffer command_buffer,
    VkQueryPool query_pool,
    uint32_t first_query,
    uint32_t query_count,
    VkBuffer destination,
    VkDeviceSize destination_offset,
    VkDeviceSize stride,
    VkQueryResultFlags flags)
{
    FakeQueryPool fake;
    if (get_fake_query_pool(query_pool, &fake)) {
        const uint32_t statistic_count =
            static_cast<uint32_t>(std::bitset<32>(fake.statistics).count());
        const bool availability = (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0;
        const uint32_t value_count = statistic_count + (availability ? 1u : 0u);
        const VkDeviceSize scalar_size =
            (flags & VK_QUERY_RESULT_64_BIT) ? sizeof(uint64_t) : sizeof(uint32_t);
        const size_t bytes = static_cast<size_t>(value_count * scalar_size);
        if (fake.cmd_update_buffer != nullptr && bytes != 0) {
            std::vector<uint8_t> payload(bytes, 0);
            if (availability) {
                if (flags & VK_QUERY_RESULT_64_BIT) {
                    const uint64_t one = 1;
                    std::memcpy(
                        payload.data() + statistic_count * scalar_size, &one, sizeof(one));
                } else {
                    const uint32_t one = 1;
                    std::memcpy(
                        payload.data() + statistic_count * scalar_size, &one, sizeof(one));
                }
            }
            for (uint32_t query = 0; query < query_count; ++query) {
                fake.cmd_update_buffer(
                    command_buffer,
                    destination,
                    destination_offset + static_cast<VkDeviceSize>(query) * stride,
                    bytes,
                    payload.data());
            }
        }
        return;
    }
    forward_query_command<PFN_vkCmdCopyQueryPoolResults>(
        query_pool, "vkCmdCopyQueryPoolResults", command_buffer, query_pool, first_query,
        query_count, destination, destination_offset, stride, flags);
}

VKAPI_ATTR VkResult VKAPI_CALL compat_vkCreateBuffer(
    VkDevice device,
    const VkBufferCreateInfo* create_info,
    const VkAllocationCallbacks* allocator,
    VkBuffer* buffer)
{
    DeviceState state;
    if (!get_device_state(device, &state)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    auto raw_create = raw_proc<PFN_vkCreateBuffer>(state, "vkCreateBuffer");
    if (raw_create == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!state.selection.unsafe || create_info == nullptr ||
        !is_sparse_buffer(create_info->flags)) {
        return raw_create(device, create_info, allocator, buffer);
    }

    VkBufferCreateInfo dense_info = *create_info;
    dense_info.flags = strip_sparse_buffer(dense_info.flags);
    VkResult result = raw_create(device, &dense_info, allocator, buffer);
    if (result != VK_SUCCESS) {
        return result;
    }
    DenseAllocation allocation;
    result = allocate_and_bind_buffer(
        state, *buffer, create_info->usage, allocator, &allocation);
    if (result != VK_SUCCESS) {
        if (auto destroy = raw_proc<PFN_vkDestroyBuffer>(state, "vkDestroyBuffer")) {
            destroy(device, *buffer, allocator);
        }
        *buffer = VK_NULL_HANDLE;
        return result;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        auto it = devices.find(device);
        if (it != devices.end()) {
            it->second.dense_buffers.emplace(*buffer, allocation);
        }
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL compat_vkDestroyBuffer(
    VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* allocator)
{
    DeviceState state;
    DenseAllocation dense;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        auto state_it = devices.find(device);
        if (state_it == devices.end()) {
            return;
        }
        state = state_it->second;
        auto it = state_it->second.dense_buffers.find(buffer);
        if (it != state_it->second.dense_buffers.end()) {
            dense = it->second;
            state_it->second.dense_buffers.erase(it);
        }
    }
    if (auto destroy = raw_proc<PFN_vkDestroyBuffer>(state, "vkDestroyBuffer")) {
        destroy(device, buffer, allocator);
    }
    if (dense.memory != VK_NULL_HANDLE) {
        if (auto free_memory = raw_proc<PFN_vkFreeMemory>(state, "vkFreeMemory")) {
            free_memory(device, dense.memory, allocator);
        }
        release_dense_budget(device, dense.size);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL compat_vkCreateImage(
    VkDevice device,
    const VkImageCreateInfo* create_info,
    const VkAllocationCallbacks* allocator,
    VkImage* image)
{
    DeviceState state;
    if (!get_device_state(device, &state)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    auto raw_create = raw_proc<PFN_vkCreateImage>(state, "vkCreateImage");
    if (raw_create == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!state.selection.unsafe || create_info == nullptr ||
        !is_sparse_image(create_info->flags)) {
        return raw_create(device, create_info, allocator, image);
    }
    if (create_info->imageType != VK_IMAGE_TYPE_2D ||
        create_info->samples != VK_SAMPLE_COUNT_1_BIT) {
        LOG_ERROR("Dense sparse emulation supports only single-sample 2D images");
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    VkImageCreateInfo dense_info = *create_info;
    dense_info.flags = strip_sparse_image(dense_info.flags);
    VkResult result = raw_create(device, &dense_info, allocator, image);
    if (result != VK_SUCCESS) {
        return result;
    }
    DenseImage dense;
    dense.create_info = *create_info;
    dense.create_info.pNext = nullptr;
    dense.create_info.pQueueFamilyIndices = nullptr;
    result = allocate_and_bind_image(state, *image, allocator, &dense.allocation);
    if (result != VK_SUCCESS) {
        if (auto destroy = raw_proc<PFN_vkDestroyImage>(state, "vkDestroyImage")) {
            destroy(device, *image, allocator);
        }
        *image = VK_NULL_HANDLE;
        return result;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        auto it = devices.find(device);
        if (it != devices.end()) {
            it->second.dense_images.emplace(*image, dense);
        }
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL compat_vkDestroyImage(
    VkDevice device, VkImage image, const VkAllocationCallbacks* allocator)
{
    DeviceState state;
    DenseAllocation dense;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        auto state_it = devices.find(device);
        if (state_it == devices.end()) {
            return;
        }
        state = state_it->second;
        auto it = state_it->second.dense_images.find(image);
        if (it != state_it->second.dense_images.end()) {
            dense = it->second.allocation;
            state_it->second.dense_images.erase(it);
        }
    }
    if (auto destroy = raw_proc<PFN_vkDestroyImage>(state, "vkDestroyImage")) {
        destroy(device, image, allocator);
    }
    if (dense.memory != VK_NULL_HANDLE) {
        if (auto free_memory = raw_proc<PFN_vkFreeMemory>(state, "vkFreeMemory")) {
            free_memory(device, dense.memory, allocator);
        }
        release_dense_budget(device, dense.size);
    }
}

bool lookup_dense_image(VkDevice device, VkImage image, DenseImage* out)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    auto state_it = devices.find(device);
    if (state_it == devices.end()) {
        return false;
    }
    auto it = state_it->second.dense_images.find(image);
    if (it == state_it->second.dense_images.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

VkSparseImageMemoryRequirements dense_sparse_requirements(const DenseImage& image)
{
    VkSparseImageMemoryRequirements requirements{};
    requirements.formatProperties = dense_sparse_format(image.create_info.format);
    requirements.imageMipTailFirstLod = image.create_info.mipLevels;
    return requirements;
}

VKAPI_ATTR void VKAPI_CALL compat_vkGetImageSparseMemoryRequirements(
    VkDevice device,
    VkImage image,
    uint32_t* requirement_count,
    VkSparseImageMemoryRequirements* requirements)
{
    DenseImage dense;
    if (!lookup_dense_image(device, image, &dense)) {
        DeviceState state;
        if (get_device_state(device, &state)) {
            if (auto raw = raw_proc<PFN_vkGetImageSparseMemoryRequirements>(
                    state, "vkGetImageSparseMemoryRequirements")) {
                raw(device, image, requirement_count, requirements);
            }
        }
        return;
    }
    if (requirement_count == nullptr) {
        return;
    }
    if (requirements == nullptr) {
        *requirement_count = 1;
    } else if (*requirement_count != 0) {
        requirements[0] = dense_sparse_requirements(dense);
        *requirement_count = 1;
    }
}

VKAPI_ATTR void VKAPI_CALL compat_vkGetImageSparseMemoryRequirements2(
    VkDevice device,
    const VkImageSparseMemoryRequirementsInfo2* info,
    uint32_t* requirement_count,
    VkSparseImageMemoryRequirements2* requirements)
{
    DenseImage dense;
    if (info == nullptr || !lookup_dense_image(device, info->image, &dense)) {
        DeviceState state;
        if (get_device_state(device, &state)) {
            if (auto raw = raw_proc<PFN_vkGetImageSparseMemoryRequirements2>(
                    state, "vkGetImageSparseMemoryRequirements2")) {
                raw(device, info, requirement_count, requirements);
            }
        }
        return;
    }
    if (requirement_count == nullptr) {
        return;
    }
    if (requirements == nullptr) {
        *requirement_count = 1;
    } else if (*requirement_count != 0) {
        requirements[0].memoryRequirements = dense_sparse_requirements(dense);
        *requirement_count = 1;
    }
}

VKAPI_ATTR void VKAPI_CALL compat_vkGetDeviceImageSparseMemoryRequirements(
    VkDevice device,
    const VkDeviceImageMemoryRequirements* info,
    uint32_t* requirement_count,
    VkSparseImageMemoryRequirements2* requirements)
{
    DeviceState state;
    if (!get_device_state(device, &state)) {
        return;
    }
    if (info == nullptr || info->pCreateInfo == nullptr || !state.selection.unsafe ||
        !is_sparse_image(info->pCreateInfo->flags)) {
        if (auto raw = raw_proc<PFN_vkGetDeviceImageSparseMemoryRequirements>(
                state, "vkGetDeviceImageSparseMemoryRequirements")) {
            raw(device, info, requirement_count, requirements);
        }
        return;
    }
    if (requirement_count == nullptr) {
        return;
    }
    if (info->pCreateInfo->imageType != VK_IMAGE_TYPE_2D ||
        info->pCreateInfo->samples != VK_SAMPLE_COUNT_1_BIT) {
        *requirement_count = 0;
        return;
    }
    if (requirements == nullptr) {
        *requirement_count = 1;
    } else if (*requirement_count != 0) {
        DenseImage dense;
        dense.create_info = *info->pCreateInfo;
        requirements[0].memoryRequirements = dense_sparse_requirements(dense);
        *requirement_count = 1;
    }
}

VKAPI_ATTR void VKAPI_CALL compat_vkGetDeviceBufferMemoryRequirements(
    VkDevice device,
    const VkDeviceBufferMemoryRequirements* info,
    VkMemoryRequirements2* requirements)
{
    DeviceState state;
    if (!get_device_state(device, &state)) {
        return;
    }
    auto raw = raw_proc<PFN_vkGetDeviceBufferMemoryRequirements>(
        state, "vkGetDeviceBufferMemoryRequirements");
    if (raw == nullptr) {
        raw = reinterpret_cast<PFN_vkGetDeviceBufferMemoryRequirements>(
            raw_proc<PFN_vkGetDeviceBufferMemoryRequirementsKHR>(
                state, "vkGetDeviceBufferMemoryRequirementsKHR"));
    }
    if (raw == nullptr || info == nullptr || info->pCreateInfo == nullptr) {
        return;
    }
    if (state.selection.unsafe && is_sparse_buffer(info->pCreateInfo->flags)) {
        VkBufferCreateInfo dense_info = *info->pCreateInfo;
        dense_info.flags = strip_sparse_buffer(dense_info.flags);
        VkDeviceBufferMemoryRequirements dense_requirements = *info;
        dense_requirements.pCreateInfo = &dense_info;
        raw(device, &dense_requirements, requirements);
    } else {
        raw(device, info, requirements);
    }
}

VKAPI_ATTR void VKAPI_CALL compat_vkGetDeviceImageMemoryRequirements(
    VkDevice device,
    const VkDeviceImageMemoryRequirements* info,
    VkMemoryRequirements2* requirements)
{
    DeviceState state;
    if (!get_device_state(device, &state)) {
        return;
    }
    auto raw = raw_proc<PFN_vkGetDeviceImageMemoryRequirements>(
        state, "vkGetDeviceImageMemoryRequirements");
    if (raw == nullptr) {
        raw = reinterpret_cast<PFN_vkGetDeviceImageMemoryRequirements>(
            raw_proc<PFN_vkGetDeviceImageMemoryRequirementsKHR>(
                state, "vkGetDeviceImageMemoryRequirementsKHR"));
    }
    if (raw == nullptr || info == nullptr || info->pCreateInfo == nullptr) {
        return;
    }
    if (state.selection.unsafe && is_sparse_image(info->pCreateInfo->flags)) {
        VkImageCreateInfo dense_info = *info->pCreateInfo;
        dense_info.flags = strip_sparse_image(dense_info.flags);
        VkDeviceImageMemoryRequirements dense_requirements = *info;
        dense_requirements.pCreateInfo = &dense_info;
        raw(device, &dense_requirements, requirements);
    } else {
        raw(device, info, requirements);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL compat_vkQueueBindSparse(
    VkQueue queue,
    uint32_t bind_info_count,
    const VkBindSparseInfo* bind_infos,
    VkFence fence)
{
    DeviceState state;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        auto queue_it = queue_devices.find(queue);
        if (queue_it == queue_devices.end()) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        auto device_it = devices.find(queue_it->second);
        if (device_it == devices.end()) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        state = device_it->second;
    }
    auto queue_submit = raw_proc<PFN_vkQueueSubmit>(state, "vkQueueSubmit");
    if (queue_submit == nullptr || (bind_info_count != 0 && bind_infos == nullptr)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!logged_sparse_approximation.exchange(true)) {
        LOG_WARN("Dense sparse emulation keeps resources fully resident and approximates "
                 "vkQueueBindSparse with semaphore-only submissions");
    }

    std::vector<VkSubmitInfo> submits(bind_info_count);
    std::vector<std::vector<VkPipelineStageFlags>> stages(bind_info_count);
    std::vector<VkTimelineSemaphoreSubmitInfo> timeline_infos(bind_info_count);
    for (uint32_t i = 0; i < bind_info_count; ++i) {
        stages[i].assign(bind_infos[i].waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        submits[i].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submits[i].pNext = nullptr;

        // Dense resources are already resident, so bind-specific pNext
        // structures are discarded. Timeline semaphore metadata applies to
        // both VkBindSparseInfo and VkSubmitInfo and must follow the translated
        // semaphore dependency.
        auto* next = static_cast<const VkBaseInStructure*>(bind_infos[i].pNext);
        while (next != nullptr) {
            if (next->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
                timeline_infos[i] =
                    *reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(next);
                timeline_infos[i].pNext = nullptr;
                submits[i].pNext = &timeline_infos[i];
                break;
            }
            next = next->pNext;
        }

        submits[i].waitSemaphoreCount = bind_infos[i].waitSemaphoreCount;
        submits[i].pWaitSemaphores = bind_infos[i].pWaitSemaphores;
        submits[i].pWaitDstStageMask = stages[i].empty() ? nullptr : stages[i].data();
        submits[i].signalSemaphoreCount = bind_infos[i].signalSemaphoreCount;
        submits[i].pSignalSemaphores = bind_infos[i].pSignalSemaphores;
    }
    return queue_submit(queue, bind_info_count, submits.data(), fence);
}

} // namespace

void register_device(VkDevice device,
                     VkPhysicalDevice physical_device,
                     ProfileSelection selection,
                     PFN_vkGetDeviceProcAddr get_device_proc_addr,
                     const VkPhysicalDeviceMemoryProperties& memory_properties)
{
    DeviceState state;
    state.device = device;
    state.physical_device = physical_device;
    state.selection = selection;
    state.gdpa = get_device_proc_addr;
    state.memory_properties = memory_properties;
    state.dense_budget = parse_budget();
    std::lock_guard<std::mutex> lock(state_mutex);
    devices[device] = state;
}

void unregister_device(VkDevice device)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    devices.erase(device);
    for (auto it = queue_devices.begin(); it != queue_devices.end();) {
        it = it->second == device ? queue_devices.erase(it) : std::next(it);
    }
    for (auto it = real_query_pool_devices.begin(); it != real_query_pool_devices.end();) {
        it = it->second == device ? real_query_pool_devices.erase(it) : std::next(it);
    }
    for (auto it = fake_query_pools.begin(); it != fake_query_pools.end();) {
        it = it->second->device == device ? fake_query_pools.erase(it) : std::next(it);
    }
}

void associate_queue(VkDevice device, VkQueue queue)
{
    if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    if (devices.find(device) != devices.end()) {
        queue_devices[queue] = device;
    }
}

PFN_vkVoidFunction get_device_hook(VkDevice device, const char* name)
{
    DeviceState state;
    if (name == nullptr || !get_device_state(device, &state) || !profile_active(state)) {
        return nullptr;
    }

#define COMPAT_HOOK(vk_name) \
    if (std::strcmp(name, #vk_name) == 0) \
        return reinterpret_cast<PFN_vkVoidFunction>(compat_##vk_name)

    COMPAT_HOOK(vkCreateQueryPool);
    COMPAT_HOOK(vkDestroyQueryPool);
    COMPAT_HOOK(vkGetQueryPoolResults);
    COMPAT_HOOK(vkCmdBeginQuery);
    COMPAT_HOOK(vkCmdEndQuery);
    COMPAT_HOOK(vkCmdResetQueryPool);
    COMPAT_HOOK(vkResetQueryPool);
    if (std::strcmp(name, "vkResetQueryPoolEXT") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(compat_vkResetQueryPool);
    }
    COMPAT_HOOK(vkCmdCopyQueryPoolResults);
    if (state.selection.unsafe) {
        COMPAT_HOOK(vkCreateBuffer);
        COMPAT_HOOK(vkDestroyBuffer);
        COMPAT_HOOK(vkCreateImage);
        COMPAT_HOOK(vkDestroyImage);
        COMPAT_HOOK(vkGetImageSparseMemoryRequirements);
        COMPAT_HOOK(vkGetImageSparseMemoryRequirements2);
        if (std::strcmp(name, "vkGetImageSparseMemoryRequirements2KHR") == 0) {
            return reinterpret_cast<PFN_vkVoidFunction>(
                compat_vkGetImageSparseMemoryRequirements2);
        }
        COMPAT_HOOK(vkGetDeviceImageSparseMemoryRequirements);
        if (std::strcmp(name, "vkGetDeviceImageSparseMemoryRequirementsKHR") == 0) {
            return reinterpret_cast<PFN_vkVoidFunction>(
                compat_vkGetDeviceImageSparseMemoryRequirements);
        }
        COMPAT_HOOK(vkGetDeviceBufferMemoryRequirements);
        COMPAT_HOOK(vkGetDeviceImageMemoryRequirements);
        if (std::strcmp(name, "vkGetDeviceBufferMemoryRequirementsKHR") == 0) {
            return reinterpret_cast<PFN_vkVoidFunction>(
                compat_vkGetDeviceBufferMemoryRequirements);
        }
        if (std::strcmp(name, "vkGetDeviceImageMemoryRequirementsKHR") == 0) {
            return reinterpret_cast<PFN_vkVoidFunction>(
                compat_vkGetDeviceImageMemoryRequirements);
        }
        COMPAT_HOOK(vkQueueBindSparse);
    }
#undef COMPAT_HOOK
    return nullptr;
}

void GraphicsPipelineTransform::prepare(
    VkDevice device,
    uint32_t create_info_count,
    const VkGraphicsPipelineCreateInfo* source)
{
    transformed_ = source;
    create_infos_.clear();
    vertex_inputs_.clear();

    DeviceState state;
    if (source == nullptr || create_info_count == 0 ||
        !get_device_state(device, &state) ||
        state.selection.profile != Profile::Vkd3d) {
        return;
    }

    create_infos_.assign(source, source + create_info_count);
    vertex_inputs_.resize(create_info_count);
    bool changed = false;
    for (uint32_t i = 0; i < create_info_count; ++i) {
        const auto* vertex_input = source[i].pVertexInputState;
        if (vertex_input == nullptr || vertex_input->pNext == nullptr) {
            continue;
        }

        const auto* base = static_cast<const VkBaseInStructure*>(vertex_input->pNext);
        const VkPipelineVertexInputDivisorStateCreateInfoKHR* divisor = nullptr;
        while (base != nullptr) {
            if (base->sType ==
                    VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR ||
                base->sType ==
                    VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT) {
                divisor = reinterpret_cast<
                    const VkPipelineVertexInputDivisorStateCreateInfoKHR*>(base);
                break;
            }
            base = base->pNext;
        }
        if (divisor == nullptr) {
            continue;
        }

        auto& owned = vertex_inputs_[i];
        owned.vertex_input = *vertex_input;
        owned.divisor.sType =
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR;
        owned.divisor.pNext = divisor->pNext;
        if (divisor->vertexBindingDivisorCount != 0 &&
            divisor->pVertexBindingDivisors != nullptr) {
            owned.descriptions.assign(
                divisor->pVertexBindingDivisors,
                divisor->pVertexBindingDivisors + divisor->vertexBindingDivisorCount);
        }
        if (state.selection.unsafe) {
            for (auto& description : owned.descriptions) {
                if (description.divisor == 0) {
                    description.divisor = std::numeric_limits<uint32_t>::max();
                }
            }
        }
        owned.divisor.vertexBindingDivisorCount =
            static_cast<uint32_t>(owned.descriptions.size());
        owned.divisor.pVertexBindingDivisors =
            owned.descriptions.empty() ? nullptr : owned.descriptions.data();
        owned.vertex_input.pNext = &owned.divisor;
        create_infos_[i].pVertexInputState = &owned.vertex_input;
        changed = true;
    }

    if (changed) {
        transformed_ = create_infos_.data();
    } else {
        create_infos_.clear();
        vertex_inputs_.clear();
    }
}

bool unsafe_sparse_enabled(VkPhysicalDevice physical_device)
{
    const auto selection = selection_for(physical_device);
    return selection.profile == Profile::Vkd3d && selection.unsafe;
}

void get_sparse_image_format_properties(
    VkPhysicalDevice physical_device,
    VkFormat format,
    VkImageType type,
    VkSampleCountFlagBits samples,
    VkImageUsageFlags,
    VkImageTiling,
    uint32_t* property_count,
    VkSparseImageFormatProperties* properties)
{
    if (property_count == nullptr) {
        return;
    }
    if (!unsafe_sparse_enabled(physical_device) ||
        type != VK_IMAGE_TYPE_2D || samples != VK_SAMPLE_COUNT_1_BIT) {
        *property_count = 0;
        return;
    }
    if (properties == nullptr) {
        *property_count = 1;
    } else if (*property_count != 0) {
        properties[0] = dense_sparse_format(format);
        *property_count = 1;
    }
}

} // namespace mali_wrapper::compatibility::device_emulation
