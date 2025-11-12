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
 * @file sdl_wayland_swapchain_wrapper.hpp
 *
 * @brief Hybrid swapchain wrapper that manages Wayland surface + SDL window lifecycle internally.
 *
 * This wrapper solves the surface lifetime issue when routing X11 surface to Wayland swapchain.
 * It creates and manages a Wayland surface + SDL window internally, forwarding all operations
 * to the underlying Wayland swapchain while ensuring proper lifecycle management.
 */

#pragma once

extern "C" {
#include <vulkan/vulkan.h>
#include <SDL2/SDL.h>
}

#include <memory>
#include "wsi/swapchain_base.hpp"
#include "wsi/synchronization.hpp"
#include "wsi/layer_utils/custom_allocator.hpp"

namespace wsi { namespace wayland {
   class surface;
   class swapchain;
}}

#include "../wayland/swapchain.hpp"
#include "../wayland/surface.hpp"

namespace wsi
{
namespace x11
{

/**
 * @brief Multi-threaded SDL Wayland swapchain with presentation threading
 *
 * This swapchain inherits from wayland::swapchain and adds multi-threading capabilities.
 * It creates and manages its own SDL window and Wayland surface internally.
 * Key features:
 * - Dedicated presentation thread for non-blocking main thread
 * - Thread-safe presentation request queue
 * - Background processing of CPU-intensive Wayland operations
 * - Proper lifecycle management of SDL components
 */
class sdl_wayland_swapchain_wrapper : public wayland::swapchain
{
public:
   /**
    * @brief Create multi-threaded SDL Wayland swapchain
    *
    * @param dev_data       Device private data
    * @param allocator      Vulkan allocation callbacks
    * @param wsi_surface    Wayland surface to use
    */
   sdl_wayland_swapchain_wrapper(wsi::device_private_data &dev_data,
                                 const VkAllocationCallbacks *allocator,
                                 wayland::surface &wsi_surface);

   virtual ~sdl_wayland_swapchain_wrapper();

   VkResult init_platform(VkDevice device, const VkSwapchainCreateInfoKHR *swapchain_create_info,
                          bool &use_presentation_thread) override;

};

} // namespace x11
} // namespace wsi