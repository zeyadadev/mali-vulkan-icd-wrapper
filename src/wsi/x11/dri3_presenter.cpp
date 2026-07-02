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

#include "dri3_presenter.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>

#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>

#include "swapchain.hpp"
#include "utils/logging.hpp"

namespace wsi
{
namespace x11
{
namespace
{

bool version_at_least(uint32_t major, uint32_t minor, uint32_t required_major, uint32_t required_minor)
{
   return major > required_major || (major == required_major && minor >= required_minor);
}

void close_dup_fds(int32_t *fds, uint32_t count)
{
   for (uint32_t i = 0; i < count; ++i)
   {
      if (fds[i] >= 0)
      {
         close(fds[i]);
         fds[i] = -1;
      }
   }
}

} // namespace

dri3_presenter::~dri3_presenter()
{
   if (m_special_event != nullptr && m_connection != nullptr)
   {
      xcb_unregister_for_special_event(m_connection, m_special_event);
      m_special_event = nullptr;
   }
}

bool dri3_presenter::is_available(xcb_connection_t *connection, surface * /*wsi_surface*/)
{
   const xcb_query_extension_reply_t *dri3_ext = xcb_get_extension_data(connection, &xcb_dri3_id);
   if (dri3_ext == nullptr || !dri3_ext->present)
   {
      WSI_LOG_INFO("DRI3 extension not available; skipping X11 DRI3 presenter.");
      return false;
   }

   const xcb_query_extension_reply_t *present_ext = xcb_get_extension_data(connection, &xcb_present_id);
   if (present_ext == nullptr || !present_ext->present)
   {
      WSI_LOG_INFO("Present extension not available; skipping X11 DRI3 presenter.");
      return false;
   }

   xcb_dri3_query_version_cookie_t dri3_cookie = xcb_dri3_query_version(connection, 1, 2);
   xcb_dri3_query_version_reply_t *dri3_reply = xcb_dri3_query_version_reply(connection, dri3_cookie, nullptr);
   if (dri3_reply == nullptr)
   {
      WSI_LOG_INFO("DRI3 version query failed; skipping X11 DRI3 presenter.");
      return false;
   }

   const bool dri3_1_2 = version_at_least(dri3_reply->major_version, dri3_reply->minor_version, 1, 2);
   WSI_LOG_DEBUG("DRI3 version %u.%u available%s.", dri3_reply->major_version, dri3_reply->minor_version,
                 dri3_1_2 ? "" : " (modifiers unavailable; single-buffer fallback only)");
   free(dri3_reply);

   xcb_present_query_version_cookie_t present_cookie = xcb_present_query_version(connection, 1, 0);
   xcb_present_query_version_reply_t *present_reply =
      xcb_present_query_version_reply(connection, present_cookie, nullptr);
   if (present_reply == nullptr)
   {
      WSI_LOG_INFO("Present version query failed; skipping X11 DRI3 presenter.");
      return false;
   }

   const bool present_ok = version_at_least(present_reply->major_version, present_reply->minor_version, 1, 0);
   WSI_LOG_DEBUG("Present version %u.%u available.", present_reply->major_version, present_reply->minor_version);
   free(present_reply);

   if (!present_ok)
   {
      WSI_LOG_INFO("Present 1.0 is required for X11 DRI3 presenter.");
      return false;
   }

   return true;
}

VkResult dri3_presenter::init(xcb_connection_t *connection, xcb_window_t window, surface *wsi_surface)
{
   m_connection = connection;
   m_window = window;
   m_wsi_surface = wsi_surface;

   xcb_dri3_query_version_cookie_t dri3_cookie = xcb_dri3_query_version(connection, 1, 2);
   xcb_dri3_query_version_reply_t *dri3_reply = xcb_dri3_query_version_reply(connection, dri3_cookie, nullptr);
   if (dri3_reply != nullptr)
   {
      m_have_dri3_1_2 = version_at_least(dri3_reply->major_version, dri3_reply->minor_version, 1, 2);
      free(dri3_reply);
   }

   m_event_id = xcb_generate_id(connection);
   m_special_event = xcb_register_for_special_xge(connection, &xcb_present_id, m_event_id, nullptr);
   if (m_special_event == nullptr)
   {
      WSI_LOG_ERROR("DRI3: failed to register Present special-event queue.");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   xcb_void_cookie_t select_cookie =
      xcb_present_select_input_checked(connection, m_event_id, window,
                                       XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY | XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY);
   xcb_generic_error_t *select_error = xcb_request_check(connection, select_cookie);
   if (select_error != nullptr)
   {
      WSI_LOG_ERROR("DRI3: failed to select Present events for window 0x%x (error=%u).",
                    static_cast<unsigned>(window), select_error->error_code);
      free(select_error);
      xcb_unregister_for_special_event(connection, m_special_event);
      m_special_event = nullptr;
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   const int flush_result = xcb_flush(connection);
   if (flush_result <= 0)
   {
      WSI_LOG_ERROR("DRI3: xcb_flush failed while initializing presenter (result=%d).", flush_result);
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   return VK_SUCCESS;
}

VkResult dri3_presenter::create_image_resources(x11_image_data *image_data, uint32_t width, uint32_t height,
                                                uint8_t depth, uint8_t bpp, uint64_t modifier)
{
   image_data->width = width;
   image_data->height = height;
   image_data->depth = depth;

   auto &external_mem = image_data->external_mem;
   const uint32_t num_planes = external_mem.get_num_planes();
   if (num_planes == 0 || num_planes > 4)
   {
      WSI_LOG_ERROR("DRI3: invalid dma-buf plane count %u.", num_planes);
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   const auto &fds = external_mem.get_buffer_fds();
   const auto &strides = external_mem.get_strides();
   const auto &offsets = external_mem.get_offsets();

   if (strides[0] < 0)
   {
      WSI_LOG_ERROR("DRI3: invalid stride %d.", strides[0]);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   image_data->stride = static_cast<uint32_t>(strides[0]);

   xcb_pixmap_t pixmap = xcb_generate_id(m_connection);
   int32_t dup_fds[4] = { -1, -1, -1, -1 };

   for (uint32_t plane = 0; plane < num_planes; ++plane)
   {
      if (fds[plane] < 0)
      {
         close_dup_fds(dup_fds, plane);
         WSI_LOG_ERROR("DRI3: missing dma-buf fd for plane %u.", plane);
         return VK_ERROR_INITIALIZATION_FAILED;
      }

      dup_fds[plane] = fcntl(fds[plane], F_DUPFD_CLOEXEC, 0);
      if (dup_fds[plane] < 0)
      {
         const int saved_errno = errno;
         close_dup_fds(dup_fds, plane);
         WSI_LOG_ERROR("DRI3: failed to duplicate dma-buf fd for plane %u: %s.", plane, strerror(saved_errno));
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }

   xcb_void_cookie_t create_cookie = {};
   bool sent_to_xcb = false;
   if (m_have_dri3_1_2)
   {
      uint32_t dri3_strides[4] = { 0, 0, 0, 0 };
      uint32_t dri3_offsets[4] = { 0, 0, 0, 0 };
      for (uint32_t plane = 0; plane < num_planes; ++plane)
      {
         if (strides[plane] < 0)
         {
            close_dup_fds(dup_fds, num_planes);
            WSI_LOG_ERROR("DRI3: invalid stride %d for plane %u.", strides[plane], plane);
            return VK_ERROR_INITIALIZATION_FAILED;
         }
         dri3_strides[plane] = static_cast<uint32_t>(strides[plane]);
         dri3_offsets[plane] = offsets[plane];
      }

      create_cookie =
         xcb_dri3_pixmap_from_buffers_checked(m_connection, pixmap, m_window, static_cast<uint8_t>(num_planes),
                                              static_cast<uint16_t>(width), static_cast<uint16_t>(height),
                                              dri3_strides[0], dri3_offsets[0], dri3_strides[1], dri3_offsets[1],
                                              dri3_strides[2], dri3_offsets[2], dri3_strides[3], dri3_offsets[3],
                                              depth, bpp, modifier, dup_fds);
      sent_to_xcb = true;
   }
   else if (num_planes == 1 && (modifier == DRM_FORMAT_MOD_INVALID || modifier == DRM_FORMAT_MOD_LINEAR) &&
            image_data->stride <= UINT16_MAX)
   {
      const uint64_t size = static_cast<uint64_t>(image_data->stride) * height;
      if (size > UINT32_MAX)
      {
         close_dup_fds(dup_fds, num_planes);
         WSI_LOG_ERROR("DRI3: single-plane pixmap size is too large (%llu).",
                       static_cast<unsigned long long>(size));
         return VK_ERROR_INITIALIZATION_FAILED;
      }

      create_cookie =
         xcb_dri3_pixmap_from_buffer_checked(m_connection, pixmap, m_window, static_cast<uint32_t>(size),
                                             static_cast<uint16_t>(width), static_cast<uint16_t>(height),
                                             static_cast<uint16_t>(image_data->stride), depth, bpp, dup_fds[0]);
      sent_to_xcb = true;
   }
   else
   {
      close_dup_fds(dup_fds, num_planes);
      WSI_LOG_ERROR("DRI3: server lacks DRI3 1.2, and image is not valid for pixmap_from_buffer fallback.");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   if (!sent_to_xcb)
   {
      close_dup_fds(dup_fds, num_planes);
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   xcb_generic_error_t *create_error = xcb_request_check(m_connection, create_cookie);
   if (create_error != nullptr)
   {
      WSI_LOG_ERROR("DRI3: failed to create pixmap from dma-buf (error=%u, depth=%u, bpp=%u, modifier=0x%llx).",
                    create_error->error_code, depth, bpp, static_cast<unsigned long long>(modifier));
      free(create_error);
      xcb_free_pixmap(m_connection, pixmap);
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   image_data->pixmap = pixmap;
   return VK_SUCCESS;
}

VkResult dri3_presenter::present_image(x11_image_data *image_data, uint32_t serial, uint64_t target_msc)
{
   if (image_data->pixmap == XCB_PIXMAP_NONE)
   {
      WSI_LOG_ERROR("DRI3: present requested without a pixmap.");
      return VK_ERROR_UNKNOWN;
   }

   const uint32_t options = m_copy_mode ? XCB_PRESENT_OPTION_COPY : XCB_PRESENT_OPTION_NONE;
   xcb_void_cookie_t present_cookie =
      xcb_present_pixmap_checked(m_connection, m_window, image_data->pixmap, serial, XCB_NONE, XCB_NONE, 0, 0,
                                 XCB_NONE, XCB_NONE, XCB_NONE, options, target_msc, 0, 0, 0, nullptr);
   xcb_discard_reply(m_connection, present_cookie.sequence);

   const int flush_result = xcb_flush(m_connection);
   if (flush_result <= 0)
   {
      WSI_LOG_ERROR("DRI3: xcb_flush failed during present (result=%d).", flush_result);
      return VK_ERROR_UNKNOWN;
   }

   return VK_SUCCESS;
}

void dri3_presenter::destroy_image_resources(x11_image_data *image_data)
{
   if (m_connection != nullptr && image_data->pixmap != XCB_PIXMAP_NONE)
   {
      xcb_free_pixmap(m_connection, image_data->pixmap);
      image_data->pixmap = XCB_PIXMAP_NONE;
   }
}

} /* namespace x11 */
} /* namespace wsi */
