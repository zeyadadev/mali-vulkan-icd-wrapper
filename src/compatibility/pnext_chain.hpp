#pragma once

#include <cstddef>
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

namespace mali_wrapper::compatibility {

// Owns a byte-for-byte copy of a Vulkan input pNext chain. Pointer members
// other than pNext remain shallow references because Vulkan only requires them
// to stay alive for the duration of the intercepted call.
class PNextChainCopy {
public:
    PNextChainCopy() = default;
    PNextChainCopy(const PNextChainCopy&) = delete;
    PNextChainCopy& operator=(const PNextChainCopy&) = delete;
    PNextChainCopy(PNextChainCopy&&) = default;
    PNextChainCopy& operator=(PNextChainCopy&&) = default;

    bool clone(const void* head);
    void* head() const;
    void* find(VkStructureType type) const;
    VkStructureType unsupported_type() const { return unsupported_type_; }

private:
    struct Allocation {
        std::unique_ptr<std::byte[]> bytes;
        std::size_t size = 0;
    };

    std::vector<Allocation> allocations_;
    VkBaseOutStructure* head_ = nullptr;
    VkStructureType unsupported_type_ = VK_STRUCTURE_TYPE_MAX_ENUM;
};

} // namespace mali_wrapper::compatibility
