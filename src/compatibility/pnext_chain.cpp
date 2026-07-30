#include "compatibility/pnext_chain.hpp"

#include <cstring>
#include <vulkan/vk_layer.h>

namespace mali_wrapper::compatibility {
namespace {

struct ForwardCompatibleBooleanFeature {
    VkStructureType sType;
    void* pNext;
    VkBool32 enabled;
};

struct ForwardCompatibleTwoBooleanFeature {
    VkStructureType sType;
    void* pNext;
    VkBool32 first;
    VkBool32 second;
};

std::size_t device_create_structure_size(VkStructureType type)
{
    if (type == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
        return sizeof(VkLayerDeviceCreateInfo);
    }
    // These two one-boolean feature structures are emitted by current vkd3d
    // headers and accepted by g29p1, while Ubuntu 24.04's Vulkan headers do not
    // yet name them. Keep the numeric ABI bridge local and bounded.
    if (static_cast<int64_t>(type) == 1000528000 || // shaderFloatControls2
        static_cast<int64_t>(type) == 1000232000 || // dynamicRenderingLocalRead
        static_cast<int64_t>(type) == 1000235000 || // shaderQuadControl
        static_cast<int64_t>(type) == 1000434000 || // shaderMaximalReconvergence
        static_cast<int64_t>(type) == 1000562000) { // maintenance7
        return sizeof(ForwardCompatibleBooleanFeature);
    }
    if (static_cast<int64_t>(type) == 1000201000) { // computeShaderDerivatives
        return sizeof(ForwardCompatibleTwoBooleanFeature);
    }
#include "compatibility/device_pnext_sizes.inc"
    return 0;
}

} // namespace

bool PNextChainCopy::clone(const void* head)
{
    allocations_.clear();
    head_ = nullptr;
    unsupported_type_ = VK_STRUCTURE_TYPE_MAX_ENUM;

    const auto* source = static_cast<const VkBaseInStructure*>(head);
    VkBaseOutStructure* previous = nullptr;

    while (source != nullptr) {
        const std::size_t size = device_create_structure_size(source->sType);
        if (size == 0) {
            unsupported_type_ = source->sType;
            allocations_.clear();
            head_ = nullptr;
            return false;
        }

        Allocation allocation;
        allocation.bytes = std::make_unique<std::byte[]>(size);
        allocation.size = size;
        std::memcpy(allocation.bytes.get(), source, size);

        auto* copied = reinterpret_cast<VkBaseOutStructure*>(allocation.bytes.get());
        copied->pNext = nullptr;
        if (previous != nullptr) {
            previous->pNext = copied;
        } else {
            head_ = copied;
        }
        previous = copied;
        allocations_.push_back(std::move(allocation));
        source = source->pNext;
    }

    return true;
}

void* PNextChainCopy::head() const
{
    return head_;
}

void* PNextChainCopy::find(VkStructureType type) const
{
    auto* current = head_;
    while (current != nullptr) {
        if (current->sType == type) {
            return current;
        }
        current = current->pNext;
    }
    return nullptr;
}

} // namespace mali_wrapper::compatibility
