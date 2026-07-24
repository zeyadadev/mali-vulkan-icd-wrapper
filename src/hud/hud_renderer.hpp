/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "hud/hud_types.hpp"

#include <memory>

#include <vulkan/vulkan.h>

namespace mali_wrapper
{
class device_private_data;
}

namespace mali_wrapper::hud
{

class HudDeviceResources;

class HudSwapchainResources
{
public:
   static bool can_attach(device_private_data &device, const VkSwapchainCreateInfoKHR &create_info,
                          bool headless) noexcept;

   static std::unique_ptr<HudSwapchainResources>
   create(device_private_data &device, VkQueue queue, const VkSwapchainCreateInfoKHR &create_info,
          const VkImage *images, uint32_t image_count, const char *display_server,
          const VkAllocationCallbacks *allocator) noexcept;

   ~HudSwapchainResources();
   HudSwapchainResources(const HudSwapchainResources &) = delete;
   HudSwapchainResources &operator=(const HudSwapchainResources &) = delete;

   VkCommandBuffer prepare(uint32_t image_index) noexcept;

private:
   struct Impl;
   explicit HudSwapchainResources(std::unique_ptr<Impl> impl);
   std::unique_ptr<Impl> implementation;
};

} // namespace mali_wrapper::hud
