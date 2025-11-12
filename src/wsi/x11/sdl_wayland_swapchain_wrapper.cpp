/*
 * Copyright (c) 2025 Arm Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file sdl_wayland_swapchain_wrapper.cpp
 *
 * @brief Implementation of hybrid swapchain wrapper for SDL Wayland routing.
 */

#include "sdl_wayland_swapchain_wrapper.hpp"
#include "../wayland/surface.hpp"
#include "../wayland/swapchain.hpp"
#include "utils/logging.hpp"

extern "C" {
#include <SDL2/SDL_syswm.h>
}

#include <cstring>

namespace wsi
{
namespace x11
{

sdl_wayland_swapchain_wrapper::sdl_wayland_swapchain_wrapper(wsi::device_private_data &dev_data,
                                                             const VkAllocationCallbacks *allocator,
                                                             wayland::surface &wsi_surface)
   : wayland::swapchain(dev_data, allocator, wsi_surface)
{
   WSI_LOG_INFO("Creating SDL Wayland swapchain with native threading support");
}

sdl_wayland_swapchain_wrapper::~sdl_wayland_swapchain_wrapper()
{
   WSI_LOG_INFO("Destroying SDL Wayland swapchain with native threading");
}

VkResult sdl_wayland_swapchain_wrapper::init_platform(VkDevice device, const VkSwapchainCreateInfoKHR *swapchain_create_info,
                                                       bool &use_presentation_thread)
{
   bool native_threading = true;

   WSI_LOG_INFO("Forcing native Wayland presentation threading to be enabled");

   VkResult result = wayland::swapchain::init_platform(device, swapchain_create_info, native_threading);
   if (result != VK_SUCCESS)
   {
      return result;
   }

   use_presentation_thread = native_threading;

   WSI_LOG_INFO("SDL Wayland swapchain initialized with native presentation threading: %s",
               native_threading ? "enabled" : "disabled");
   return VK_SUCCESS;
}


} // namespace x11
} // namespace wsi