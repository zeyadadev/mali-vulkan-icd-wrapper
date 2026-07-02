/*
 * Copyright (c) 2026 Arm Limited.
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

#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/xcb.h>

namespace wsi
{
namespace x11
{

class surface;
struct x11_image_data;

/**
 * @brief X11 DRI3 + Present presenter.
 *
 * Wraps each swapchain dma-buf as an X pixmap through DRI3, then submits that
 * pixmap through the Present extension. The swapchain owns image selection and
 * event handling; this class owns extension negotiation, pixmap creation, and
 * Present submission.
 */
class dri3_presenter
{
public:
   dri3_presenter() = default;
   ~dri3_presenter();

   bool is_available(xcb_connection_t *connection, surface *wsi_surface);
   VkResult init(xcb_connection_t *connection, xcb_window_t window, surface *wsi_surface);

   VkResult create_image_resources(x11_image_data *image_data, uint32_t width, uint32_t height, uint8_t depth,
                                   uint8_t bpp, uint64_t modifier);
   VkResult present_image(x11_image_data *image_data, uint32_t serial, uint64_t target_msc);
   void destroy_image_resources(x11_image_data *image_data);

   xcb_special_event_t *get_present_special_event() const
   {
      return m_special_event;
   }

   bool supports_modifiers() const
   {
      return m_have_dri3_1_2;
   }

   void set_copy_mode(bool enable)
   {
      m_copy_mode = enable;
   }

private:
   xcb_connection_t *m_connection = nullptr;
   xcb_window_t m_window = XCB_WINDOW_NONE;
   surface *m_wsi_surface = nullptr;

   xcb_present_event_t m_event_id = 0;
   xcb_special_event_t *m_special_event = nullptr;

   bool m_have_dri3_1_2 = false;
   bool m_copy_mode = false;
};

} /* namespace x11 */
} /* namespace wsi */
