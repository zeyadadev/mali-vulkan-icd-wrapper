/*
 * Copyright (c) 2017-2022 Arm Limited.
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
 * @file swapchain.cpp
 *
 * @brief Contains the implementation for a x11 swapchain.
 */

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <system_error>
#include <thread>

#include <dlfcn.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <link.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../layer_utils/timed_semaphore.hpp"
#include <vulkan/vulkan_core.h>

#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

#include "drm_display.hpp"
#include "dri3_presenter.hpp"
#include "swapchain.hpp"
#include "utils/logging.hpp"
#include "../layer_utils/drm/drm_utils.hpp"
#include "../layer_utils/format_modifiers.hpp"
#include "../layer_utils/macros.hpp"
#include "wsi/external_memory.hpp"
#include "wsi/swapchain_base.hpp"
#include "wsi/extensions/present_id.hpp"
#include "shm_presenter.hpp"
#include "xwayland_dmabuf_bridge.hpp"

namespace wsi
{
namespace x11
{

#define X11_SWAPCHAIN_MAX_PENDING_COMPLETIONS 128

namespace
{
std::atomic<bool> g_disable_xwayland_bridge_runtime{ false };

bool env_var_is_enabled(const char *value)
{
   return value && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
}

bool allow_x11_non_fifo_present_mode()
{
   return env_var_is_enabled(std::getenv("WSI_ALLOW_NON_FIFO_PRESENT_MODE"));
}

bool allow_legacy_bridge_non_fifo_present_mode()
{
   if (env_var_is_enabled(std::getenv("XWL_DMABUF_BRIDGE_ALLOW_MAILBOX")))
   {
      static std::atomic<bool> warned_legacy_mailbox_env{ false };
      bool expected = false;
      if (warned_legacy_mailbox_env.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
      {
         WSI_LOG_WARNING("XWL_DMABUF_BRIDGE_ALLOW_MAILBOX is deprecated; use WSI_ALLOW_NON_FIFO_PRESENT_MODE=1.");
      }
      return true;
   }

   return false;
}

struct x11_dri3_format
{
   uint32_t xwayland_fourcc;
   VkFormat allocation_vk_format;
   uint8_t depth;
   uint8_t bpp;
};

std::optional<x11_dri3_format> x11_dri3_format_for_depth(int depth)
{
   switch (depth)
   {
   case 16:
      return x11_dri3_format{ DRM_FORMAT_RGB565, VK_FORMAT_R5G6B5_UNORM_PACK16, 16, 16 };
   case 24:
      /* Xwayland imports depth-24 pixmaps as XRGB8888. Allocate ARGB8888, which is layout-compatible. */
      return x11_dri3_format{ DRM_FORMAT_XRGB8888, VK_FORMAT_B8G8R8A8_UNORM, 24, 32 };
   case 30:
      return x11_dri3_format{ DRM_FORMAT_ARGB2101010, VK_FORMAT_A2R10G10B10_UNORM_PACK32, 30, 32 };
   case 32:
      return x11_dri3_format{ DRM_FORMAT_ARGB8888, VK_FORMAT_B8G8R8A8_UNORM, 32, 32 };
   default:
      return std::nullopt;
   }
}

bool modifier_in_list(const util::vector<uint64_t> &modifiers, uint64_t modifier)
{
   return std::find(modifiers.begin(), modifiers.end(), modifier) != modifiers.end();
}

VkResult push_unique_modifier(util::vector<uint64_t> &modifiers, uint64_t modifier)
{
   if (modifier_in_list(modifiers, modifier))
   {
      return VK_SUCCESS;
   }

   if (!modifiers.try_push_back(modifier))
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   return VK_SUCCESS;
}

VkResult add_dri3_fallback_modifiers(util::vector<uint64_t> &modifiers)
{
   TRY_LOG_CALL(push_unique_modifier(modifiers, DRM_FORMAT_MOD_LINEAR));
   TRY_LOG_CALL(push_unique_modifier(modifiers, DRM_FORMAT_MOD_INVALID));
   return VK_SUCCESS;
}

VkResult query_dri3_supported_modifiers(xcb_connection_t *connection, xcb_window_t window, uint8_t depth, uint8_t bpp,
                                        bool query_server, util::vector<uint64_t> &modifiers)
{
   if (query_server)
   {
      xcb_dri3_get_supported_modifiers_cookie_t cookie =
         xcb_dri3_get_supported_modifiers(connection, window, depth, bpp);
      xcb_dri3_get_supported_modifiers_reply_t *reply =
         xcb_dri3_get_supported_modifiers_reply(connection, cookie, nullptr);

      if (reply != nullptr)
      {
         const uint64_t *window_modifiers = xcb_dri3_get_supported_modifiers_window_modifiers(reply);
         const int window_modifier_count = xcb_dri3_get_supported_modifiers_window_modifiers_length(reply);
         for (int i = 0; i < window_modifier_count; ++i)
         {
            TRY_LOG_CALL(push_unique_modifier(modifiers, window_modifiers[i]));
         }

         const uint64_t *screen_modifiers = xcb_dri3_get_supported_modifiers_screen_modifiers(reply);
         const int screen_modifier_count = xcb_dri3_get_supported_modifiers_screen_modifiers_length(reply);
         for (int i = 0; i < screen_modifier_count; ++i)
         {
            TRY_LOG_CALL(push_unique_modifier(modifiers, screen_modifiers[i]));
         }

         free(reply);
      }
   }

   if (modifiers.empty())
   {
      TRY_LOG_CALL(add_dri3_fallback_modifiers(modifiers));
   }

   return VK_SUCCESS;
}

VkResult filter_dri3_importable_formats(const util::vector<wsialloc_format> &source_formats,
                                        const util::vector<uint64_t> &server_modifiers, uint32_t allocation_fourcc,
                                        util::vector<wsialloc_format> &filtered_formats)
{
   for (uint64_t server_modifier : server_modifiers)
   {
      for (const auto &format : source_formats)
      {
         if (format.fourcc == allocation_fourcc && format.modifier == server_modifier)
         {
            if (!filtered_formats.try_push_back(format))
            {
               return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
         }
      }
   }

   return VK_SUCCESS;
}

bool is_lsfg_vk_loaded()
{
   struct search_context
   {
      bool found;
   } context{ false };

   dl_iterate_phdr(
      [](struct dl_phdr_info *info, size_t, void *data) {
         auto *context = static_cast<search_context *>(data);
         if (info && info->dlpi_name && std::strstr(info->dlpi_name, "liblsfg-vk") != nullptr)
         {
            context->found = true;
            return 1;
         }
         return 0;
      },
      &context);

   return context.found;
}

size_t bridge_reserved_free_image_count()
{
   const char *reserved_env = std::getenv("XWL_DMABUF_BRIDGE_RESERVED_FREE_IMAGES");
   if (reserved_env && reserved_env[0] != '\0')
   {
      errno = 0;
      char *end = nullptr;
      unsigned long parsed = std::strtoul(reserved_env, &end, 10);
      if (errno == 0 && end != reserved_env && *end == '\0' && parsed > 0)
      {
         return static_cast<size_t>(parsed);
      }

      WSI_LOG_WARNING("Xwayland bridge: invalid XWL_DMABUF_BRIDGE_RESERVED_FREE_IMAGES='%s', using automatic value.",
                      reserved_env);
   }

   return is_lsfg_vk_loaded() ? 2u : 1u;
}

size_t bridge_release_lag_for_image_count(size_t image_count)
{
   const size_t reserved_free_images = bridge_reserved_free_image_count();
   return (image_count > reserved_free_images) ? (image_count - reserved_free_images) : 1u;
}

void validate_bridge_plane_sizes_once(x11_image_data &image_data)
{
   auto &external_mem = image_data.external_mem;
   const auto &offsets = external_mem.get_offsets();
   const auto &strides = external_mem.get_strides();
   const auto &fds = external_mem.get_buffer_fds();

   for (uint32_t plane = 0; plane < external_mem.get_num_planes(); ++plane)
   {
      const int plane_fd = fds[plane];
      const uint64_t required_size = static_cast<uint64_t>(offsets[plane]) +
                                     static_cast<uint64_t>(strides[plane]) * image_data.height;

      struct stat fd_stat = {};
      if (plane_fd < 0 || fstat(plane_fd, &fd_stat) != 0)
      {
         WSI_LOG_WARNING("Xwayland bridge: plane[%u] fd=%d stat failed during setup (required_size=%llu): %s", plane,
                         plane_fd, static_cast<unsigned long long>(required_size), strerror(errno));
         continue;
      }

      WSI_LOG_DEBUG("Xwayland bridge: validated plane[%u] fd=%d offset=%u stride=%d height=%u required_size=%llu fd_size=%llu",
                    plane, plane_fd, offsets[plane], strides[plane], image_data.height,
                    static_cast<unsigned long long>(required_size),
                    static_cast<unsigned long long>(fd_stat.st_size));
      if (required_size > static_cast<uint64_t>(fd_stat.st_size))
      {
         WSI_LOG_WARNING("Xwayland bridge: plane[%u] required_size (%llu) exceeds fd_size (%llu) during setup", plane,
                         static_cast<unsigned long long>(required_size),
                         static_cast<unsigned long long>(fd_stat.st_size));
      }
   }
}
} // namespace

static VkResult fill_image_create_info(VkImageCreateInfo &image_create_info,
                                       util::vector<VkSubresourceLayout> &image_plane_layouts,
                                       VkImageDrmFormatModifierExplicitCreateInfoEXT &drm_mod_info,
                                       VkExternalMemoryImageCreateInfoKHR &external_info, x11_image_data &image_data,
                                       uint64_t modifier)
{
   TRY_LOG_CALL(image_data.external_mem.fill_image_plane_layouts(image_plane_layouts));

   if (image_data.external_mem.is_disjoint())
   {
      image_create_info.flags |= VK_IMAGE_CREATE_DISJOINT_BIT;
   }

   image_data.external_mem.fill_drm_mod_info(image_create_info.pNext, drm_mod_info, image_plane_layouts, modifier);
   image_data.external_mem.fill_external_info(external_info, &drm_mod_info);
   image_create_info.pNext = &external_info;
   image_create_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
   return VK_SUCCESS;
}

swapchain::swapchain(wsi::device_private_data &dev_data, const VkAllocationCallbacks *pAllocator,
                     surface &wsi_surface)
   : swapchain_base(dev_data, pAllocator)
   , m_connection(wsi_surface.get_connection())
   , m_window(wsi_surface.get_window())
   , m_wsi_surface(&wsi_surface)
   , m_wsi_allocator(nullptr)
   , m_image_creation_parameters({}, m_allocator, {}, {})
   , m_send_sbc(0)
   , m_target_msc(0)
   , m_last_present_msc(0)
   , m_present_event_thread_run(false)
   , m_thread_status_lock()
   , m_thread_status_cond()
{
   m_image_create_info.format = VK_FORMAT_UNDEFINED;
}

swapchain::~swapchain()
{
   WSI_LOG_DEBUG("x11::swapchain destructor begin this=%p window=0x%x use_dri3=%s use_bridge=%s", (void *)this,
                 static_cast<unsigned>(m_window), m_use_dri3_presenter ? "true" : "false",
                 m_use_xwayland_bridge ? "true" : "false");
   auto thread_status_lock = std::unique_lock<std::mutex>(m_thread_status_lock);

   if (m_present_event_thread_run)
   {
      m_present_event_thread_run = false;
      m_thread_status_cond.notify_all();
   }

   const bool join_present_event_thread = m_present_event_thread.joinable();
   thread_status_lock.unlock();

   if (join_present_event_thread)
   {
      m_present_event_thread.join();
   }

   thread_status_lock.lock();
   /* Keep bridge teardown serialized with present_image(), which uses the same lock. */

   if (m_use_xwayland_bridge)
   {
      while (!m_bridge_pending_unpresent.empty())
      {
         unpresent_image(m_bridge_pending_unpresent.front());
         m_bridge_pending_unpresent.pop_front();
      }
   }

   if (m_use_xwayland_bridge && m_xwayland_bridge)
   {
      m_xwayland_bridge->stop_stream(m_window);
   }

   thread_status_lock.unlock();

   /* Call the base's teardown */
   teardown();
   WSI_LOG_DEBUG("x11::swapchain destructor end this=%p", (void *)this);
}

VkResult swapchain::init_platform(VkDevice device, const VkSwapchainCreateInfoKHR *swapchain_create_info,
                                  bool &use_presentation_thread)
{
   UNUSED(device);
   UNUSED(swapchain_create_info);
   m_device_data.instance_data.disp.GetPhysicalDeviceMemoryProperties2KHR(m_device_data.physical_device,
                                                                          &m_memory_props);
   if (m_wsi_surface == nullptr)
   {
      WSI_LOG_ERROR("X11 swapchain init_platform: m_wsi_surface is null");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   WSIALLOC_ASSERT_VERSION();
   if (wsialloc_new(&m_wsi_allocator) != WSIALLOC_ERROR_NONE)
   {
      WSI_LOG_ERROR("Failed to create wsi allocator.");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   const bool force_shm = env_var_is_enabled(std::getenv("WSI_X11_FORCE_SHM"));
   const bool force_bridge = env_var_is_enabled(std::getenv("WSI_X11_FORCE_BRIDGE"));
   const bool dri3_copy = env_var_is_enabled(std::getenv("WSI_X11_DRI3_COPY"));

   m_xwayland_bridge = xwayland_dmabuf_bridge_client::create_from_environment();
   const bool bridge_configured = (m_xwayland_bridge != nullptr) && m_xwayland_bridge->is_enabled();
   const bool bridge_requested = bridge_configured && m_xwayland_bridge->is_available();
   const bool bridge_runtime_disabled = g_disable_xwayland_bridge_runtime.load(std::memory_order_acquire);
   const bool bridge_available = bridge_requested && (!bridge_runtime_disabled || force_bridge);

   if (bridge_configured && !bridge_requested)
   {
      WSI_LOG_WARNING(
         "Xwayland bridge is configured but unavailable during swapchain creation; using DRI3/SHM fallback.");
   }

   WSI_LOG_INFO(
      "X11 presentation path probe: force_shm=%s force_bridge=%s dri3_copy=%s bridge_configured=%s "
      "bridge_requested=%s bridge_runtime_disabled=%s bridge_available=%s",
      force_shm ? "yes" : "no", force_bridge ? "yes" : "no", dri3_copy ? "yes" : "no",
      bridge_configured ? "yes" : "no", bridge_requested ? "yes" : "no",
      bridge_runtime_disabled ? "yes" : "no", bridge_available ? "yes" : "no");

   auto init_shm_presenter = [&]() -> VkResult {
      try
      {
         m_shm_presenter = std::make_unique<shm_presenter>();

         if (!m_shm_presenter->is_available(m_connection, m_wsi_surface))
         {
            WSI_LOG_ERROR("SHM presenter is not available");
            return VK_ERROR_INITIALIZATION_FAILED;
         }

         VkResult init_result = m_shm_presenter->init(m_connection, m_window, m_wsi_surface);
         if (init_result != VK_SUCCESS)
         {
            WSI_LOG_ERROR("Failed to initialize SHM presenter");
            return init_result;
         }
      }
      catch (const std::exception &e)
      {
         WSI_LOG_ERROR("Exception creating SHM presenter: %s", e.what());
         return VK_ERROR_INITIALIZATION_FAILED;
      }

      WSI_LOG_INFO("X11 swapchain using SHM presentation path%s", force_shm ? " (forced)" : "");
      return VK_SUCCESS;
   };

   if (force_shm)
   {
      TRY_LOG_CALL(init_shm_presenter());
   }
   else if (force_bridge && bridge_available)
   {
      m_use_xwayland_bridge = true;
      WSI_LOG_INFO("WSI_X11_FORCE_BRIDGE set: using Xwayland dmabuf bridge presentation path.");
   }
   else
   {
      if (force_bridge && !bridge_available)
      {
         WSI_LOG_WARNING("WSI_X11_FORCE_BRIDGE set, but XWL_DMABUF_BRIDGE is unavailable; trying DRI3/SHM fallback.");
      }
      if (bridge_requested && bridge_runtime_disabled && !force_bridge)
      {
         WSI_LOG_WARNING("Xwayland bridge disabled after a previous runtime failure; preferring DRI3/SHM fallback.");
      }

      try
      {
         m_dri3_presenter = std::make_unique<dri3_presenter>();
         if (m_dri3_presenter->is_available(m_connection, m_wsi_surface))
         {
            m_dri3_presenter->set_copy_mode(dri3_copy);
            VkResult init_result = m_dri3_presenter->init(m_connection, m_window, m_wsi_surface);
            if (init_result == VK_SUCCESS)
            {
               m_use_dri3_presenter = true;
               WSI_LOG_INFO("X11 swapchain using DRI3 Present path (%s).", dri3_copy ? "copy" : "zero-copy");
            }
            else
            {
               WSI_LOG_WARNING("DRI3 presenter init failed (%d); trying fallback presentation path.", init_result);
               m_dri3_presenter.reset();
            }
         }
         else
         {
            m_dri3_presenter.reset();
         }
      }
      catch (const std::exception &e)
      {
         WSI_LOG_WARNING("Exception creating DRI3 presenter: %s", e.what());
         m_dri3_presenter.reset();
      }

      if (!m_use_dri3_presenter)
      {
         if (bridge_available)
         {
            m_use_xwayland_bridge = true;
            WSI_LOG_INFO("DRI3 unavailable: using Xwayland dmabuf bridge presentation path.");
         }
         else
         {
            TRY_LOG_CALL(init_shm_presenter());
         }
      }
   }

   const char *selected_path =
      m_use_dri3_presenter ? "DRI3" : (m_use_xwayland_bridge ? "BRIDGE" : "SHM");
   WSI_LOG_INFO("X11 presentation path selected=%s window=0x%x", selected_path,
                static_cast<unsigned>(m_window));

   const bool requested_non_fifo_mode =
      m_present_mode == VK_PRESENT_MODE_MAILBOX_KHR || m_present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR;
   const bool allow_non_fifo_mode = allow_x11_non_fifo_present_mode();
   if (m_use_dri3_presenter && requested_non_fifo_mode && !allow_non_fifo_mode)
   {
      WSI_LOG_WARNING(
         "X11 DRI3 Present: forcing FIFO present mode for stability (set WSI_ALLOW_NON_FIFO_PRESENT_MODE=1 to keep requested mode).");
      m_present_mode = VK_PRESENT_MODE_FIFO_KHR;
   }
   else if (m_use_xwayland_bridge && requested_non_fifo_mode &&
            !(allow_non_fifo_mode || allow_legacy_bridge_non_fifo_present_mode()))
   {
      WSI_LOG_WARNING(
         "Xwayland bridge: forcing FIFO present mode for safety (set WSI_ALLOW_NON_FIFO_PRESENT_MODE=1 to keep requested mode).");
      m_present_mode = VK_PRESENT_MODE_FIFO_KHR;
   }

   if (m_use_xwayland_bridge)
   {
      constexpr size_t bridge_target_image_count = wsi::surface_properties::MAX_SWAPCHAIN_IMAGE_COUNT;
      if (m_swapchain_images.size() < bridge_target_image_count)
      {
         if (!m_swapchain_images.try_resize(bridge_target_image_count))
         {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
         }
         WSI_LOG_INFO("Xwayland bridge: increasing swapchain image count to %zu for safer dmabuf reuse",
                      m_swapchain_images.size());
      }

      init_bridge_present_rate_limit();
   }

   /*
    * swapchain_base initializes m_error_state to VK_NOT_READY until its init()
    * finishes. The X11 Present event thread starts from init_platform(), before
    * that final base step, so mark the platform side healthy before launching
    * the thread. Otherwise a fast-starting thread can treat the initial
    * VK_NOT_READY sentinel as a fatal swapchain error.
    */
   set_error_state(VK_SUCCESS);

   {
      auto thread_status_lock = std::unique_lock<std::mutex>(m_thread_status_lock);
      m_present_event_thread_run = true;
   }

   try
   {
      m_present_event_thread = std::thread(&swapchain::present_event_thread, this);
   }
   catch (const std::system_error &)
   {
      auto thread_status_lock = std::unique_lock<std::mutex>(m_thread_status_lock);
      m_present_event_thread_run = false;
      m_thread_status_cond.notify_all();
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   catch (const std::bad_alloc &)
   {
      auto thread_status_lock = std::unique_lock<std::mutex>(m_thread_status_lock);
      m_present_event_thread_run = false;
      m_thread_status_cond.notify_all();
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   /*
    * When VK_PRESENT_MODE_MAILBOX_KHR has been chosen by the application we don't
    * initialize the page flip thread so the present_image function can be called
    * during vkQueuePresent.
    */
   use_presentation_thread = (m_present_mode != VK_PRESENT_MODE_MAILBOX_KHR);

   return VK_SUCCESS;
}

void swapchain::init_bridge_present_rate_limit()
{
   constexpr uint32_t max_supported_fps = 240;

   bool has_env_override = false;
   uint32_t fps = 0;

   const char *fps_env = std::getenv("XWL_DMABUF_BRIDGE_MAX_FPS");
   if (fps_env && fps_env[0] != '\0')
   {
      has_env_override = true;
      errno = 0;
      char *end = nullptr;
      unsigned long parsed = std::strtoul(fps_env, &end, 10);

      if (errno != 0 || end == fps_env || *end != '\0')
      {
         WSI_LOG_WARNING("Xwayland bridge: invalid XWL_DMABUF_BRIDGE_MAX_FPS='%s', leaving timer pacing disabled.",
                         fps_env);
      }
      else if (parsed > max_supported_fps)
      {
         fps = max_supported_fps;
      }
      else
      {
         fps = static_cast<uint32_t>(parsed);
      }
   }
   m_bridge_present_fps_override = has_env_override;

   if (!has_env_override)
   {
      m_bridge_present_interval_ns = 0;
      m_bridge_present_rate_limit_initialized = false;
      WSI_LOG_INFO("Xwayland bridge: timer pacing disabled by default; set XWL_DMABUF_BRIDGE_MAX_FPS to enable a cap.");
      return;
   }

   if (fps == 0)
   {
      m_bridge_present_interval_ns = 0;
      m_bridge_present_rate_limit_initialized = false;
      if (has_env_override)
      {
         WSI_LOG_INFO("Xwayland bridge: present pacing disabled (XWL_DMABUF_BRIDGE_MAX_FPS=0).");
      }
      return;
   }

   m_bridge_present_interval_ns = 1000000000ull / fps;
   m_bridge_next_present_time = std::chrono::steady_clock::now();
   m_bridge_present_rate_limit_initialized = true;
   WSI_LOG_INFO("Xwayland bridge: present pacing enabled at %u FPS", fps);

   if (!m_bridge_present_fps_override && m_xwayland_bridge && m_xwayland_bridge->is_feedback_sync_enabled())
   {
      WSI_LOG_INFO(
         "Xwayland bridge: sync feedback active; enforcing timer cap as an additional bridge safety bound.");
   }
}

void swapchain::throttle_bridge_present_if_needed()
{
   if (!m_use_xwayland_bridge || m_bridge_present_interval_ns == 0)
   {
      return;
   }

   if (!m_bridge_present_rate_limit_initialized)
   {
      m_bridge_next_present_time = std::chrono::steady_clock::now();
      m_bridge_present_rate_limit_initialized = true;
      return;
   }

   const auto interval = std::chrono::nanoseconds(m_bridge_present_interval_ns);
   const auto now = std::chrono::steady_clock::now();
   if (now < m_bridge_next_present_time)
   {
      std::this_thread::sleep_until(m_bridge_next_present_time);
   }

   const auto after_wait = std::chrono::steady_clock::now();
   m_bridge_next_present_time = after_wait + interval;
}

VkResult swapchain::get_surface_compatible_formats(const VkImageCreateInfo &info,
                                                   util::vector<wsialloc_format> &importable_formats,
                                                   util::vector<uint64_t> &exportable_modifers,
                                                   util::vector<VkDrmFormatModifierPropertiesEXT> &drm_format_props,
                                                   bool require_drm_display_support)
{
   TRY_LOG(util::get_drm_format_properties(m_device_data.physical_device, info.format, drm_format_props),
           "Failed to get format properties");

   std::optional<drm_display> *display = nullptr;
   if (require_drm_display_support)
   {
      auto &display_ref = drm_display::get_display();
      if (!display_ref.has_value())
      {
         WSI_LOG_ERROR("DRM display not available.");
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
      display = &display_ref;
   }

   for (const auto &prop : drm_format_props)
   {
      drm_format_pair drm_format{ util::drm::vk_to_drm_format(info.format), prop.drmFormatModifier };

      if (require_drm_display_support && !display->value().is_format_supported(drm_format))
      {
         continue;
      }

      VkExternalImageFormatPropertiesKHR external_props = {};
      external_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES_KHR;

      VkImageFormatProperties2KHR format_props = {};
      format_props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2_KHR;
      format_props.pNext = &external_props;

      VkResult result = VK_SUCCESS;
      {
         VkPhysicalDeviceExternalImageFormatInfoKHR external_info = {};
         external_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO_KHR;
         external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

         VkPhysicalDeviceImageDrmFormatModifierInfoEXT drm_mod_info = {};
         drm_mod_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
         drm_mod_info.pNext = &external_info;
         drm_mod_info.drmFormatModifier = prop.drmFormatModifier;
         drm_mod_info.sharingMode = info.sharingMode;
         drm_mod_info.queueFamilyIndexCount = info.queueFamilyIndexCount;
         drm_mod_info.pQueueFamilyIndices = info.pQueueFamilyIndices;

         VkPhysicalDeviceImageFormatInfo2KHR image_info = {};
         image_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2_KHR;
         image_info.pNext = &drm_mod_info;
         image_info.format = info.format;
         image_info.type = info.imageType;
         image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
         image_info.usage = info.usage;
         image_info.flags = info.flags;

#if WSI_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN
         VkImageCompressionControlEXT compression_control = {};
         compression_control.sType = VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT;
         compression_control.flags = m_image_compression_control_params.flags;
         compression_control.compressionControlPlaneCount =
            m_image_compression_control_params.compression_control_plane_count;
         compression_control.pFixedRateFlags = m_image_compression_control_params.fixed_rate_flags.data();

         if (m_device_data.is_swapchain_compression_control_enabled())
         {
            compression_control.pNext = image_info.pNext;
            image_info.pNext = &compression_control;
         }
#endif
         result = m_device_data.instance_data.disp.GetPhysicalDeviceImageFormatProperties2KHR(
            m_device_data.physical_device, &image_info, &format_props);
      }
      if (result != VK_SUCCESS)
      {
         continue;
      }
      if (format_props.imageFormatProperties.maxExtent.width < info.extent.width ||
          format_props.imageFormatProperties.maxExtent.height < info.extent.height ||
          format_props.imageFormatProperties.maxExtent.depth < info.extent.depth)
      {
         continue;
      }
      if (format_props.imageFormatProperties.maxMipLevels < info.mipLevels ||
          format_props.imageFormatProperties.maxArrayLayers < info.arrayLayers)
      {
         continue;
      }
      if ((format_props.imageFormatProperties.sampleCounts & info.samples) != info.samples)
      {
         continue;
      }

      if (external_props.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT_KHR)
      {
         if (!exportable_modifers.try_push_back(drm_format.modifier))
         {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
         }
      }

      if (external_props.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT_KHR)
      {
         uint64_t flags =
            (prop.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_DISJOINT_BIT) ? 0 : WSIALLOC_FORMAT_NON_DISJOINT;
         wsialloc_format import_format{ drm_format.fourcc, drm_format.modifier, flags };
         if (!importable_formats.try_push_back(import_format))
         {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
         }
      }
   }

   return VK_SUCCESS;
}

VkResult swapchain::allocate_wsialloc(VkImageCreateInfo &image_create_info, x11_image_data *image_data,
                                      util::vector<wsialloc_format> &importable_formats,
                                      wsialloc_format *allocated_format, bool avoid_allocation)
{
   bool is_protected_memory = (image_create_info.flags & VK_IMAGE_CREATE_PROTECTED_BIT) != 0;
   uint64_t allocation_flags = is_protected_memory ? WSIALLOC_ALLOCATE_PROTECTED : 0;
   if (avoid_allocation)
   {
      allocation_flags |= WSIALLOC_ALLOCATE_NO_MEMORY;
   }

#if WSI_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN
   if (m_image_compression_control_params.flags & VK_IMAGE_COMPRESSION_FIXED_RATE_EXPLICIT_EXT)
   {
      allocation_flags |= WSIALLOC_ALLOCATE_HIGHEST_FIXED_RATE_COMPRESSION;
   }
#endif

   wsialloc_allocate_info alloc_info = { importable_formats.data(), static_cast<unsigned>(importable_formats.size()),
                                         image_create_info.extent.width, image_create_info.extent.height,
                                         allocation_flags };

   wsialloc_allocate_result alloc_result = { {}, { 0 }, { 0 }, { -1 }, false };
   /* Clear buffer_fds and average_row_strides for error purposes */
   for (int i = 0; i < WSIALLOC_MAX_PLANES; ++i)
   {
      alloc_result.buffer_fds[i] = -1;
      alloc_result.average_row_strides[i] = -1;
   }
   const auto res = wsialloc_alloc(m_wsi_allocator, &alloc_info, &alloc_result);
   if (res != WSIALLOC_ERROR_NONE)
   {
      WSI_LOG_ERROR("Failed allocation of DMA Buffer. WSI error: %d", static_cast<int>(res));
      if (res == WSIALLOC_ERROR_NOT_SUPPORTED)
      {
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
      }
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   *allocated_format = alloc_result.format;
   auto &external_memory = image_data->external_mem;
   external_memory.set_strides(alloc_result.average_row_strides);
   external_memory.set_buffer_fds(alloc_result.buffer_fds);
   external_memory.set_offsets(alloc_result.offsets);

   uint32_t num_planes = util::drm::drm_fourcc_format_get_num_planes(alloc_result.format.fourcc);

   if (!avoid_allocation)
   {
      uint32_t num_memory_planes = 0;

      for (uint32_t i = 0; i < num_planes; ++i)
      {
         auto it = std::find(std::begin(alloc_result.buffer_fds) + i + 1, std::end(alloc_result.buffer_fds),
                             alloc_result.buffer_fds[i]);
         if (it == std::end(alloc_result.buffer_fds))
         {
            num_memory_planes++;
         }
      }

      assert(alloc_result.is_disjoint == (num_memory_planes > 1));
      external_memory.set_num_memories(num_memory_planes);
   }

   external_memory.set_format_info(alloc_result.is_disjoint, num_planes);
   external_memory.set_memory_handle_type(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
   return VK_SUCCESS;
}

VkResult swapchain::allocate_image(VkImageCreateInfo &image_create_info, x11_image_data *image_data)
{
   UNUSED(image_create_info);
   util::vector<wsialloc_format> importable_formats(util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));
   auto &m_allocated_format = m_image_creation_parameters.m_allocated_format;
   if (!importable_formats.try_push_back(m_allocated_format))
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   TRY_LOG_CALL(allocate_wsialloc(m_image_create_info, image_data, importable_formats, &m_allocated_format, false));

   return VK_SUCCESS;
}

VkResult swapchain::allocate_and_bind_swapchain_image(VkImageCreateInfo image_create_info, swapchain_image &image)
{
   std::unique_lock<std::recursive_mutex> image_status_lock(m_image_status_mutex);
   image.status = swapchain_image::FREE;

   assert(image.data != nullptr);
   auto image_data = static_cast<x11_image_data *>(image.data);

   if (m_use_xwayland_bridge || m_use_dri3_presenter)
   {
      image_data->width = image_create_info.extent.width;
      image_data->height = image_create_info.extent.height;
      uint32_t dummy_w = 0;
      uint32_t dummy_h = 0;
      int depth = 24;
      if (!m_wsi_surface->get_size_and_depth(&dummy_w, &dummy_h, &depth))
      {
         WSI_LOG_WARNING("Could not get surface depth, using default: %d", depth);
      }
      auto x11_format = x11_dri3_format_for_depth(depth);
      image_data->depth = x11_format ? static_cast<int>(x11_format->depth) : depth;
      image_data->bpp = x11_format ? x11_format->bpp : static_cast<uint8_t>((depth == 24) ? 32 : depth);
      image_data->modifier = m_image_creation_parameters.m_allocated_format.modifier;

      TRY_LOG(allocate_image(m_image_create_info, image_data), "Failed to allocate image");
      if (m_use_xwayland_bridge)
      {
         validate_bridge_plane_sizes_once(*image_data);
      }
      image_status_lock.unlock();

      TRY_LOG(image_data->external_mem.import_memory_and_bind_swapchain_image(image.image),
              "Failed to import memory and bind swapchain image");

      if (m_use_dri3_presenter)
      {
         TRY_LOG(m_dri3_presenter->create_image_resources(image_data, image_data->width, image_data->height,
                                                          image_data->depth, image_data->bpp, image_data->modifier),
                 "Failed to create DRI3 presentation image resources");
      }

      /* Initialize presentation fence. */
      auto present_fence = sync_fd_fence_sync::create(m_device_data);
      if (!present_fence.has_value())
      {
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
      image_data->present_fence = std::move(present_fence.value());

      return VK_SUCCESS;
   }

   image_status_lock.unlock();

   uint32_t width = image_create_info.extent.width;
   uint32_t height = image_create_info.extent.height;

   int depth = 24; // Default depth, may need to be queried from surface later
   uint32_t dummy_w, dummy_h;
   if (!m_wsi_surface->get_size_and_depth(&dummy_w, &dummy_h, &depth))
   {
      WSI_LOG_WARNING("Could not get surface depth, using default: %d", depth);
   }

   TRY_LOG(m_shm_presenter->create_image_resources(image_data, width, height, depth),
           "Failed to create presentation image resources");

   /* Initialize presentation fence. */
   auto present_fence = sync_fd_fence_sync::create(m_device_data);
   if (!present_fence.has_value())
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   image_data->present_fence = std::move(present_fence.value());

   return VK_SUCCESS;
}

VkResult swapchain::create_swapchain_image(VkImageCreateInfo image_create_info, swapchain_image &image)
{
   /* Create image_data */
   auto image_data = m_allocator.create<x11_image_data>(1, m_device, m_allocator);
   if (image_data == nullptr)
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   image.data = image_data;
   image_data->device = m_device;
   image_data->device_data = &m_device_data;

   if (m_use_xwayland_bridge || m_use_dri3_presenter)
   {
      if (m_image_create_info.format == VK_FORMAT_UNDEFINED)
      {
         VkImageCreateInfo dmabuf_image_create_info = image_create_info;
         util::vector<wsialloc_format> importable_formats(
            util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));
         util::vector<uint64_t> exportable_modifiers(util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));

         util::vector<VkDrmFormatModifierPropertiesEXT> drm_format_props(
            util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));

         std::optional<x11_dri3_format> x11_format;
         uint32_t allocation_fourcc = 0;
         if (m_use_dri3_presenter)
         {
            uint32_t surface_width = 0;
            uint32_t surface_height = 0;
            int depth = 24;
            if (!m_wsi_surface->get_size_and_depth(&surface_width, &surface_height, &depth))
            {
               WSI_LOG_WARNING("DRI3: could not query X11 surface depth, using default depth %d.", depth);
            }

            x11_format = x11_dri3_format_for_depth(depth);
            if (!x11_format.has_value())
            {
               WSI_LOG_ERROR("DRI3: unsupported X11 window depth %d.", depth);
               return VK_ERROR_FORMAT_NOT_SUPPORTED;
            }

            dmabuf_image_create_info.format = x11_format->allocation_vk_format;
            allocation_fourcc = util::drm::vk_to_drm_format(dmabuf_image_create_info.format);
            if (allocation_fourcc == 0)
            {
               WSI_LOG_ERROR("DRI3: no DRM fourcc mapping for Vulkan format %u.",
                             static_cast<unsigned>(dmabuf_image_create_info.format));
               return VK_ERROR_FORMAT_NOT_SUPPORTED;
            }
         }

         TRY_LOG_CALL(
            get_surface_compatible_formats(dmabuf_image_create_info, importable_formats, exportable_modifiers,
                                           drm_format_props, false));

         if (m_use_dri3_presenter)
         {
            util::vector<uint64_t> server_modifiers(util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));
            TRY_LOG_CALL(query_dri3_supported_modifiers(m_connection, m_window, x11_format->depth, x11_format->bpp,
                                                        m_dri3_presenter->supports_modifiers(), server_modifiers));

            util::vector<wsialloc_format> dri3_formats(
               util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));
            TRY_LOG_CALL(
               filter_dri3_importable_formats(importable_formats, server_modifiers, allocation_fourcc, dri3_formats));

            if (dri3_formats.empty())
            {
               util::vector<uint64_t> fallback_modifiers(
                  util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));
               TRY_LOG_CALL(add_dri3_fallback_modifiers(fallback_modifiers));
               TRY_LOG_CALL(filter_dri3_importable_formats(importable_formats, fallback_modifiers, allocation_fourcc,
                                                           dri3_formats));
            }

            importable_formats.clear();
            for (const auto &format : dri3_formats)
            {
               if (!importable_formats.try_push_back(format))
               {
                  return VK_ERROR_OUT_OF_HOST_MEMORY;
               }
            }
         }

         if (importable_formats.empty())
         {
            WSI_LOG_ERROR("No importable dmabuf formats available for X11 %s path.",
                          m_use_dri3_presenter ? "DRI3" : "bridge");
            return VK_ERROR_INITIALIZATION_FAILED;
         }

         if (m_use_dri3_presenter)
         {
            WSI_LOG_INFO(
               "DRI3: importable dmabuf candidates=%zu x11_fourcc=0x%x allocation_fourcc=0x%x depth=%u bpp=%u",
               importable_formats.size(), x11_format->xwayland_fourcc, allocation_fourcc, x11_format->depth,
               x11_format->bpp);
         }
         else
         {
            WSI_LOG_INFO("Xwayland bridge: importable dmabuf candidates=%zu", importable_formats.size());
         }
         constexpr size_t max_logged_candidates = 8;
         for (size_t idx = 0; idx < importable_formats.size() && idx < max_logged_candidates; ++idx)
         {
            const auto &candidate = importable_formats[idx];
            WSI_LOG_INFO("X11 dmabuf: candidate[%zu] fourcc=0x%x modifier=0x%llx", idx, candidate.fourcc,
                         static_cast<unsigned long long>(candidate.modifier));
         }
         if (importable_formats.size() > max_logged_candidates)
         {
            WSI_LOG_INFO("X11 dmabuf: ... %zu more candidates not shown",
                         importable_formats.size() - max_logged_candidates);
         }

         wsialloc_format allocated_format = { 0, 0, 0 };
         const char *prefer_linear_env = std::getenv("XWL_DMABUF_BRIDGE_PREFER_LINEAR");
         const bool prefer_linear = prefer_linear_env && !(prefer_linear_env[0] == '0' && prefer_linear_env[1] == '\0');

         bool forced_linear = false;
         bool preferred_non_linear = false;
         if (m_use_dri3_presenter)
         {
            TRY_LOG_CALL(
               allocate_wsialloc(dmabuf_image_create_info, image_data, importable_formats, &allocated_format, true));
         }
         else if (prefer_linear)
         {
            auto linear_it = std::find_if(importable_formats.begin(), importable_formats.end(),
                                          [](const wsialloc_format &fmt) { return fmt.modifier == DRM_FORMAT_MOD_LINEAR; });
            if (linear_it != importable_formats.end())
            {
               util::vector<wsialloc_format> linear_only(
                  util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));
               if (!linear_only.try_push_back(*linear_it))
               {
                  return VK_ERROR_OUT_OF_HOST_MEMORY;
               }
               TRY_LOG_CALL(allocate_wsialloc(dmabuf_image_create_info, image_data, linear_only, &allocated_format, true));
               forced_linear = true;
            }
            else
            {
               WSI_LOG_WARNING("Xwayland bridge: DRM_FORMAT_MOD_LINEAR unavailable, falling back to allocator default.");
               TRY_LOG_CALL(
                  allocate_wsialloc(dmabuf_image_create_info, image_data, importable_formats, &allocated_format, true));
            }
         }
         else
         {
            util::vector<wsialloc_format> non_linear_formats(
               util::allocator(m_allocator, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));

            for (const auto &fmt : importable_formats)
            {
               if (fmt.modifier == DRM_FORMAT_MOD_LINEAR)
               {
                  continue;
               }
               if (!non_linear_formats.try_push_back(fmt))
               {
                  return VK_ERROR_OUT_OF_HOST_MEMORY;
               }
            }

            if (!non_linear_formats.empty())
            {
               TRY_LOG_CALL(
                  allocate_wsialloc(dmabuf_image_create_info, image_data, non_linear_formats, &allocated_format, true));
               preferred_non_linear = true;
            }
            else
            {
               WSI_LOG_WARNING(
                  "Xwayland bridge: non-linear modifiers unavailable, falling back to allocator default (may pick linear).");
               TRY_LOG_CALL(
                  allocate_wsialloc(dmabuf_image_create_info, image_data, importable_formats, &allocated_format, true));
            }
         }

         WSI_LOG_INFO("X11 dmabuf: selected fourcc=0x%x modifier=0x%llx%s%s%s",
                      allocated_format.fourcc, static_cast<unsigned long long>(allocated_format.modifier),
                      m_use_dri3_presenter ? " (DRI3)" : " (bridge)",
                      forced_linear ? " (linear forced)" : "",
                      preferred_non_linear ? " (non-linear preferred)" : "");
         if (m_use_xwayland_bridge && allocated_format.fourcc == DRM_FORMAT_ARGB8888)
         {
            WSI_LOG_INFO("Xwayland bridge: presentation fourcc remap enabled 0x%x -> 0x%x",
                         DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888);
         }
         else if (m_use_xwayland_bridge && allocated_format.fourcc == DRM_FORMAT_ABGR8888)
         {
            WSI_LOG_INFO("Xwayland bridge: presentation fourcc remap enabled 0x%x -> 0x%x",
                         DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888);
         }

         for (auto &prop : drm_format_props)
         {
            if (prop.drmFormatModifier == allocated_format.modifier)
            {
               image_data->external_mem.set_num_memories(prop.drmFormatModifierPlaneCount);
            }
         }

         TRY_LOG_CALL(fill_image_create_info(
            dmabuf_image_create_info, m_image_creation_parameters.m_image_layout,
            m_image_creation_parameters.m_drm_mod_info,
            m_image_creation_parameters.m_external_info, *image_data, allocated_format.modifier));

         m_image_create_info = dmabuf_image_create_info;
         m_image_creation_parameters.m_allocated_format = allocated_format;
      }

      return m_device_data.disp.CreateImage(m_device, &m_image_create_info, get_allocation_callbacks(), &image.image);
   }

   if (m_shm_presenter)
   {
      VkMemoryPropertyFlags optimal = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                      VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
      VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

      TRY_LOG_CALL(image_data->external_mem.configure_for_host_visible(image_create_info, required, optimal));

      image_create_info.tiling = VK_IMAGE_TILING_LINEAR;
      TRY_LOG(m_device_data.disp.CreateImage(m_device, &image_create_info, get_allocation_callbacks(), &image.image),
              "Failed to create image for SHM");

      return image_data->external_mem.allocate_and_bind_image(image.image, image_create_info);
   }

   return allocate_image(image_create_info, image_data);
}

void swapchain::present_event_thread()
{
   auto thread_status_lock = std::unique_lock<std::mutex>(m_thread_status_lock);

   if (m_use_dri3_presenter)
   {
      xcb_special_event_t *present_event = m_dri3_presenter ? m_dri3_presenter->get_present_special_event() : nullptr;
      while (m_present_event_thread_run)
      {
         thread_status_lock.unlock();
         xcb_generic_event_t *event =
            present_event ? xcb_poll_for_special_event(m_connection, present_event) : nullptr;
         thread_status_lock.lock();

         if (event != nullptr)
         {
            auto *generic = reinterpret_cast<xcb_present_generic_event_t *>(event);
            if (generic->evtype == XCB_PRESENT_IDLE_NOTIFY)
            {
               auto *idle = reinterpret_cast<xcb_present_idle_notify_event_t *>(event);
               if (!m_free_buffer_pool.push_back(idle->pixmap))
               {
                  WSI_LOG_ERROR("DRI3: free buffer pool full, dropping idle pixmap 0x%x.",
                                static_cast<unsigned>(idle->pixmap));
               }
               m_thread_status_cond.notify_all();
            }
            else if (generic->evtype == XCB_PRESENT_COMPLETE_NOTIFY)
            {
               auto *complete = reinterpret_cast<xcb_present_complete_notify_event_t *>(event);
               m_last_present_msc = complete->msc;

               for (auto &image : m_swapchain_images)
               {
                  if (image.status == swapchain_image::INVALID || image.data == nullptr)
                  {
                     continue;
                  }

                  auto *data = reinterpret_cast<x11_image_data *>(image.data);
                  auto completion_it = std::find_if(data->pending_completions.begin(), data->pending_completions.end(),
                                                    [complete](const pending_completion &completion) {
                                                       return completion.serial == complete->serial;
                                                    });
                  if (completion_it != data->pending_completions.end())
                  {
                     if (m_device_data.is_present_id_enabled())
                     {
                        auto *ext = get_swapchain_extension<wsi_ext_present_id>(true);
                        ext->set_present_id(completion_it->present_id);
                     }
                     data->pending_completions.erase(completion_it);
                     break;
                  }
               }
               m_thread_status_cond.notify_all();
            }
            else
            {
               WSI_LOG_DEBUG("DRI3: ignoring Present event type %u.", generic->evtype);
            }

            free(event);
            continue;
         }

         const int xcb_error = xcb_connection_has_error(m_connection);
         if (xcb_error != 0)
         {
            WSI_LOG_ERROR("DRI3 Present event thread exiting due to XCB connection error %d.", xcb_error);
            set_error_state(VK_ERROR_SURFACE_LOST_KHR);
            break;
         }

         if (error_has_occured())
         {
            WSI_LOG_ERROR("DRI3 Present event thread exiting due to swapchain error state %d.", get_error_state());
            break;
         }

         m_thread_status_cond.wait_for(thread_status_lock, std::chrono::milliseconds(2));
      }

      m_present_event_thread_run = false;
      m_thread_status_cond.notify_all();
      return;
   }

   while (m_present_event_thread_run)
   {
      auto assume_forward_progress = false;

      for (auto &image : m_swapchain_images)
      {
         if (image.status == swapchain_image::INVALID)
            continue;

         auto data = reinterpret_cast<x11_image_data *>(image.data);
         if (data->pending_completions.size() != 0)
         {
            assume_forward_progress = true;
            break;
         }
      }

      if (!assume_forward_progress)
      {
         m_thread_status_cond.wait(thread_status_lock);
         continue;
      }

      if (error_has_occured())
      {
         break;
      }

      thread_status_lock.unlock();

      thread_status_lock.lock();
      std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Short polling interval
   }

   m_present_event_thread_run = false;
   m_thread_status_cond.notify_all();
}

void swapchain::present_image(const pending_present_request &pending_present)
{
   auto image_data = reinterpret_cast<x11_image_data *>(m_swapchain_images[pending_present.image_index].data);
   auto thread_status_lock = std::unique_lock<std::mutex>(m_thread_status_lock);

   while (image_data->pending_completions.size() == X11_SWAPCHAIN_MAX_PENDING_COMPLETIONS)
   {
      if (!m_present_event_thread_run)
      {
         if (m_device_data.is_present_id_enabled())
         {
            auto *ext = get_swapchain_extension<wsi_ext_present_id>(true);
            ext->set_present_id(pending_present.present_id);
         }
         return unpresent_image(pending_present.image_index);
      }
      m_thread_status_cond.wait(thread_status_lock);
   }

   m_send_sbc++;
   uint32_t serial = (uint32_t)m_send_sbc;

   uint64_t target_msc = 0;
   if (m_use_dri3_presenter &&
       (m_present_mode == VK_PRESENT_MODE_FIFO_KHR || m_present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR))
   {
      m_target_msc++;
      if (m_last_present_msc + 1 > m_target_msc)
      {
         m_target_msc = m_last_present_msc + 1;
      }
      target_msc = m_target_msc;
   }

   VkResult present_result = VK_SUCCESS;
   if (m_use_xwayland_bridge)
   {
      auto &external_mem = image_data->external_mem;
      uint32_t bridge_fourcc = m_image_creation_parameters.m_allocated_format.fourcc;
      if (bridge_fourcc == DRM_FORMAT_ARGB8888)
      {
         bridge_fourcc = DRM_FORMAT_XRGB8888;
      }
      else if (bridge_fourcc == DRM_FORMAT_ABGR8888)
      {
         bridge_fourcc = DRM_FORMAT_XBGR8888;
      }

      const auto &offsets = external_mem.get_offsets();
      const auto &strides = external_mem.get_strides();
      const auto &fds = external_mem.get_buffer_fds();

      const bool bridge_ok =
         m_xwayland_bridge &&
         m_xwayland_bridge->present_frame(static_cast<uint32_t>(m_window), image_data->width, image_data->height,
                                          bridge_fourcc,
                                          m_image_creation_parameters.m_allocated_format.modifier,
                                          external_mem.get_num_planes(), offsets.data(), strides.data(), fds.data());

      if (!bridge_ok)
      {
         WSI_LOG_WARNING("Xwayland bridge submit failed: window=0x%x image=%u size=%ux%u format=0x%x modifier=0x%llx",
                         static_cast<unsigned>(m_window), pending_present.image_index, image_data->width,
                         image_data->height, bridge_fourcc,
                         static_cast<unsigned long long>(m_image_creation_parameters.m_allocated_format.modifier));
         present_result = VK_ERROR_OUT_OF_DATE_KHR;
         set_error_state(VK_ERROR_OUT_OF_DATE_KHR);

         const bool was_disabled = g_disable_xwayland_bridge_runtime.exchange(true, std::memory_order_acq_rel);
         if (!was_disabled)
         {
            WSI_LOG_WARNING(
               "Disabling Xwayland bridge for this process due to runtime failure. Recreate swapchain to continue on SHM path.");
         }
      }
   }
   else if (m_use_dri3_presenter)
   {
      try
      {
         image_data->pending_completions.push_back({ serial, pending_present.present_id, std::nullopt });
      }
      catch (const std::bad_alloc &)
      {
         present_result = VK_ERROR_OUT_OF_HOST_MEMORY;
      }

      if (present_result == VK_SUCCESS)
      {
         present_result = m_dri3_presenter->present_image(image_data, serial, target_msc);
         if (present_result != VK_SUCCESS && !image_data->pending_completions.empty())
         {
            image_data->pending_completions.pop_back();
         }
      }
   }
   else
   {
      present_result = m_shm_presenter->present_image(image_data, serial);
   }

   if (present_result != VK_SUCCESS)
   {
      WSI_LOG_ERROR("Failed to present image on X11 swapchain path: %d", present_result);
   }

   if (!m_use_dri3_presenter && m_device_data.is_present_id_enabled())
   {
      auto *ext = get_swapchain_extension<wsi_ext_present_id>(true);
      ext->set_present_id(pending_present.present_id);
   }

   uint32_t image_index_to_unpresent = 0;
   bool should_unpresent = false;

   if (m_use_xwayland_bridge && present_result == VK_SUCCESS)
   {
      /* Keep buffers unavailable to acquire until bridge pacing has been applied. */
      thread_status_lock.unlock();
      throttle_bridge_present_if_needed();
      thread_status_lock.lock();
   }

   if (m_use_dri3_presenter)
   {
      if (present_result != VK_SUCCESS)
      {
         WSI_LOG_ERROR("DRI3 present failed with result %d, releasing image immediately", present_result);
         image_index_to_unpresent = pending_present.image_index;
         should_unpresent = true;
      }
   }
   else if (!m_use_xwayland_bridge)
   {
      image_index_to_unpresent = pending_present.image_index;
      should_unpresent = true;
   }
   else if (present_result == VK_SUCCESS)
   {
      /*
       * Do not release the just-submitted image immediately on bridge path.
       * Keep a small in-flight queue so we do not render into a buffer that
       * may still be sampled by the compositor.
       */
      const size_t release_lag_frames = bridge_release_lag_for_image_count(m_swapchain_images.size());
      if (!m_bridge_release_lag_logged)
      {
         WSI_LOG_INFO("Xwayland bridge: delayed image release enabled (lag=%zu frame%s, swapchain_images=%zu)",
                      release_lag_frames, (release_lag_frames == 1) ? "" : "s", m_swapchain_images.size());
         m_bridge_release_lag_logged = true;
      }
      m_bridge_pending_unpresent.push_back(pending_present.image_index);

      while (m_bridge_pending_unpresent.size() > release_lag_frames)
      {
         const uint32_t completed_index = m_bridge_pending_unpresent.front();
         m_bridge_pending_unpresent.pop_front();
         unpresent_image(completed_index);
      }
   }
   else
   {
      WSI_LOG_ERROR("Present failed with result %d, performing immediate cleanup", present_result);
      image_index_to_unpresent = pending_present.image_index;
      should_unpresent = true;

      while (!m_bridge_pending_unpresent.empty())
      {
         unpresent_image(m_bridge_pending_unpresent.front());
         m_bridge_pending_unpresent.pop_front();
      }
   }

   m_thread_status_cond.notify_all();

   thread_status_lock.unlock();

   if (should_unpresent)
   {
      unpresent_image(image_index_to_unpresent);
   }
}

bool swapchain::free_image_found()
{
   while (m_free_buffer_pool.size() > 0)
   {
      auto pixmap = m_free_buffer_pool.pop_front();
      assert(pixmap.has_value());
      for (size_t i = 0; i < m_swapchain_images.size(); i++)
      {
         auto &image = m_swapchain_images[i];
         if (image.status == swapchain_image::INVALID || image.data == nullptr)
         {
            continue;
         }

         auto data = reinterpret_cast<x11_image_data *>(image.data);
         if (data->pixmap == pixmap.value())
         {
            unpresent_image(i);
         }
      }
   }

   for (auto &img : m_swapchain_images)
   {
      if (img.status == swapchain_image::FREE)
      {
         return true;
      }
   }
   return false;
}

VkResult swapchain::get_free_buffer(uint64_t *timeout)
{
   auto thread_status_lock = std::unique_lock<std::mutex>(m_thread_status_lock);

   if (*timeout == 0)
   {
      return free_image_found() ? VK_SUCCESS : VK_NOT_READY;
   }
   else if (*timeout == UINT64_MAX)
   {
      while (!free_image_found())
      {
         if (error_has_occured())
         {
            return get_error_state();
         }

         if (!m_present_event_thread_run)
         {
            WSI_LOG_DEBUG("X11 present event thread stopped while waiting for a free swapchain image.");
            return VK_ERROR_OUT_OF_DATE_KHR;
         }

         m_thread_status_cond.wait(thread_status_lock);
      }
   }
   else
   {
      auto time_point = std::chrono::high_resolution_clock::now() + std::chrono::nanoseconds(*timeout);

      while (!free_image_found())
      {
         if (error_has_occured())
         {
            return get_error_state();
         }

         if (!m_present_event_thread_run)
         {
            WSI_LOG_DEBUG("X11 present event thread stopped while waiting for a free swapchain image.");
            return VK_ERROR_OUT_OF_DATE_KHR;
         }

         if (m_thread_status_cond.wait_until(thread_status_lock, time_point) == std::cv_status::timeout)
         {
            return VK_TIMEOUT;
         }
      }
   }

   *timeout = 0;
   return VK_SUCCESS;
}

void swapchain::destroy_image(wsi::swapchain_image &image)
{
   std::unique_lock<std::recursive_mutex> image_status_lock(m_image_status_mutex);
   if (image.status != wsi::swapchain_image::INVALID)
   {
      if (image.image != VK_NULL_HANDLE)
      {
         m_device_data.disp.DestroyImage(m_device, image.image, get_allocation_callbacks());
         image.image = VK_NULL_HANDLE;
      }

      image.status = wsi::swapchain_image::INVALID;
   }

   image_status_lock.unlock();

   if (image.data != nullptr)
   {
      auto data = reinterpret_cast<x11_image_data *>(image.data);

      if (m_shm_presenter && data != nullptr)
      {
         m_shm_presenter->destroy_image_resources(data);
      }

      if (m_dri3_presenter && data != nullptr)
      {
         m_dri3_presenter->destroy_image_resources(data);
      }

      m_allocator.destroy(1, data);
      image.data = nullptr;
   }
}

VkResult swapchain::image_set_present_payload(swapchain_image &image, VkQueue queue,
                                              const queue_submit_semaphores &semaphores, const void *submission_pnext)
{
   auto data = reinterpret_cast<x11_image_data *>(image.data);
   return data->present_fence.set_payload(queue, semaphores, submission_pnext);
}

VkResult swapchain::image_wait_present(swapchain_image &image, uint64_t timeout)
{
   auto data = reinterpret_cast<x11_image_data *>(image.data);
   return data->present_fence.wait_payload(timeout);
}

VkResult swapchain::bind_swapchain_image(VkDevice &device, const VkBindImageMemoryInfo *bind_image_mem_info,
                                         const VkBindImageMemorySwapchainInfoKHR *bind_sc_info)
{
   UNUSED(device);
   const wsi::swapchain_image &swapchain_image = m_swapchain_images[bind_sc_info->imageIndex];
   auto image_data = reinterpret_cast<x11_image_data *>(swapchain_image.data);
   return image_data->external_mem.bind_swapchain_image_memory(bind_image_mem_info->image);
}

VkResult swapchain::add_required_extensions(VkDevice device, const VkSwapchainCreateInfoKHR *swapchain_create_info)
{
   UNUSED(device);
   UNUSED(swapchain_create_info);

   if (m_device_data.is_present_id_enabled())
   {
      if (!add_swapchain_extension(m_allocator.make_unique<wsi_ext_present_id>()))
      {
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }

   return VK_SUCCESS;
}

} /* namespace x11 */
} /* namespace wsi */
