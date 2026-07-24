/*
 * SPDX-License-Identifier: MIT
 */

#include "hud/hud_runtime.hpp"

#include "utils/logging.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace mali_wrapper::hud
{

HudRuntime &HudRuntime::instance() noexcept
{
   static HudRuntime runtime;
   return runtime;
}

HudRuntime::HudRuntime() noexcept
   : settings(load_config())
{
}

void HudRuntime::capture_application(const VkApplicationInfo *application_info) noexcept
{
   if (!enabled())
   {
      return;
   }
   try
   {
      const ApplicationMetadata copy = copy_application_metadata(application_info);
      std::lock_guard<std::mutex> lock(lifecycle_mutex);
      const int old_priority = application_metadata_priority(application_metadata);
      const int new_priority = application_metadata_priority(copy);
      if (new_priority >= old_priority)
      {
         application_metadata = copy;
      }
      if (settings.debug)
      {
         std::fprintf(stderr,
                      "mali-hud: Vulkan application=\"%s\" engine=\"%s\" engineVersion=%u.%u.%u "
                      "stack=%u priority=%d%s\n",
                      copy.application_name.data(), copy.engine_name.data(),
                      VK_VERSION_MAJOR(copy.engine_version), VK_VERSION_MINOR(copy.engine_version),
                      VK_VERSION_PATCH(copy.engine_version), static_cast<unsigned>(copy.stack),
                      new_priority, new_priority >= old_priority ? "" : " (kept earlier metadata)");
      }
   }
   catch (...)
   {
      log_disable_once("application metadata capture failed");
   }
}

ApplicationMetadata HudRuntime::application() const noexcept
{
   try
   {
      std::lock_guard<std::mutex> lock(lifecycle_mutex);
      return application_metadata;
   }
   catch (...)
   {
      return {};
   }
}

uint64_t HudRuntime::register_swapchain(uint32_t width, uint32_t height, bool visible) noexcept
{
   if (!enabled() || !visible)
   {
      return 0;
   }

   try
   {
      const uint64_t id = next_id.fetch_add(1, std::memory_order_relaxed);
      std::lock_guard<std::mutex> lock(lifecycle_mutex);
      const bool first = swapchains.empty();
      swapchains.push_back({ id, static_cast<uint64_t>(width) * static_cast<uint64_t>(height), visible });
      choose_primary_locked();
      if (first && !sampler.start(settings.interval_ms, frame_tracker, settings.test_fault == HudFault::sensor))
      {
         log_disable_once("metrics thread could not be started; metrics will remain unavailable");
      }
      return id;
   }
   catch (...)
   {
      log_disable_once("swapchain registration failed");
      return 0;
   }
}

void HudRuntime::unregister_swapchain(uint64_t id) noexcept
{
   if (id == 0)
   {
      return;
   }

   try
   {
      std::lock_guard<std::mutex> lock(lifecycle_mutex);
      swapchains.erase(std::remove_if(swapchains.begin(), swapchains.end(),
                                      [id](const SwapchainRecord &record) { return record.id == id; }),
                       swapchains.end());
      choose_primary_locked();
      if (swapchains.empty())
      {
         sampler.stop();
      }
   }
   catch (...)
   {
      log_disable_once("swapchain unregistration failed");
   }
}

void HudRuntime::choose_primary_locked() noexcept
{
   uint64_t chosen_id = 0;
   uint64_t chosen_area = 0;
   for (const SwapchainRecord &record : swapchains)
   {
      if (record.visible && record.area >= chosen_area)
      {
         chosen_area = record.area;
         chosen_id = record.id;
      }
   }
   const uint64_t previous = primary_id.exchange(chosen_id, std::memory_order_relaxed);
   if (previous != chosen_id)
   {
      frame_tracker.reset();
   }
}

void HudRuntime::log_disable_once(const char *reason) noexcept
{
   if (!settings.debug)
   {
      return;
   }
   bool expected = false;
   if (disable_logged.compare_exchange_strong(expected, true, std::memory_order_relaxed))
   {
      try
      {
         const char *message = reason == nullptr ? "disabled" : reason;
         std::fprintf(stderr, "mali-hud: %s\n", message);
         LOG_WARN(std::string("Mali HUD: ") + message);
      }
      catch (...)
      {
         // Diagnostics must never make a HUD failure fatal.
      }
   }
}

} // namespace mali_wrapper::hud
