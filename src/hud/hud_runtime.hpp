/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "hud/hud_config.hpp"
#include "hud/hud_metadata.hpp"
#include "hud/hud_metrics.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace mali_wrapper::hud
{

class HudRuntime
{
public:
   static HudRuntime &instance() noexcept;

   const HudConfig &config() const noexcept
   {
      return settings;
   }
   bool enabled() const noexcept
   {
      return settings.enabled;
   }

   void capture_application(const VkApplicationInfo *application_info) noexcept;
   ApplicationMetadata application() const noexcept;

   uint64_t register_swapchain(uint32_t width, uint32_t height, bool visible) noexcept;
   void unregister_swapchain(uint64_t id) noexcept;
   bool is_primary(uint64_t id) const noexcept
   {
      return primary_id.load(std::memory_order_relaxed) == id;
   }
   void present(uint64_t id) noexcept
   {
      if (is_primary(id))
      {
         frame_tracker.present();
      }
   }
   MetricsSnapshot metrics(uint64_t *generation = nullptr) const noexcept
   {
      return sampler.snapshot(generation);
   }
   void log_disable_once(const char *reason) noexcept;

private:
   struct SwapchainRecord
   {
      uint64_t id;
      uint64_t area;
      bool visible;
   };

   HudRuntime() noexcept;
   void choose_primary_locked() noexcept;

   HudConfig settings;
   mutable std::mutex lifecycle_mutex;
   ApplicationMetadata application_metadata{};
   std::vector<SwapchainRecord> swapchains;
   std::atomic<uint64_t> next_id{ 1 };
   std::atomic<uint64_t> primary_id{ 0 };
   std::atomic<bool> disable_logged{ false };
   FrameTracker frame_tracker;
   MetricSampler sampler;
};

} // namespace mali_wrapper::hud
