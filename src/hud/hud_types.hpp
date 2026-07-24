/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace mali_wrapper::hud
{

constexpr size_t HUD_TEXT_SHORT = 64;
constexpr size_t HUD_TEXT_LONG = 192;

enum class HudPosition : uint8_t
{
   top_left,
   top_right,
   bottom_left,
   bottom_right,
};

enum class HudFault : uint8_t
{
   none,
   atlas,
   buffer,
   pipeline,
   sensor,
};

struct HudConfig
{
   bool enabled{ false };
   bool debug{ false };
   bool automatic_scale{ true };
   HudPosition position{ HudPosition::top_left };
   HudFault test_fault{ HudFault::none };
   float scale{ 1.0f };
   float opacity{ 0.55f };
   float text_opacity{ 0.90f };
   uint32_t interval_ms{ 500 };
};

enum class ApiStackKind : uint8_t
{
   native_vulkan,
   zink,
   dxvk,
   vkd3d,
};

enum class DxvkClientApi : uint8_t
{
   unknown,
   d3d8,
   d3d9,
   d3d10,
   d3d11,
};

struct ApplicationMetadata
{
   std::array<char, HUD_TEXT_SHORT> executable{};
   std::array<char, HUD_TEXT_SHORT> application_name{};
   std::array<char, HUD_TEXT_SHORT> engine_name{};
   std::array<char, HUD_TEXT_SHORT> translation_version{};
   ApiStackKind stack{ ApiStackKind::native_vulkan };
   DxvkClientApi dxvk_client{ DxvkClientApi::unknown };
   bool stack_from_module_scan{ false };
   uint32_t application_version{ 0 };
   uint32_t engine_version{ 0 };
   uint32_t api_version{ 0 };
};

struct DriverMetadata
{
   std::array<char, HUD_TEXT_SHORT> device_name{};
   std::array<char, HUD_TEXT_SHORT> driver_name{};
   std::array<char, HUD_TEXT_LONG> driver_info{};
   uint32_t api_version{ 0 };
   uint32_t driver_version{ 0 };
   uint32_t driver_id{ 0 };
};

struct MetricsSnapshot
{
   uint64_t monotonic_ns{ 0 };
   float fps{ std::numeric_limits<float>::quiet_NaN() };
   float frame_time_ms{ std::numeric_limits<float>::quiet_NaN() };
   float system_cpu_percent{ std::numeric_limits<float>::quiet_NaN() };
   float process_cpu_percent{ std::numeric_limits<float>::quiet_NaN() };
   float cpu_temp_c{ std::numeric_limits<float>::quiet_NaN() };
   float system_ram_used_gib{ std::numeric_limits<float>::quiet_NaN() };
   float system_ram_total_gib{ std::numeric_limits<float>::quiet_NaN() };
   float process_ram_mib{ std::numeric_limits<float>::quiet_NaN() };
   float gpu_percent{ std::numeric_limits<float>::quiet_NaN() };
   float gpu_clock_mhz{ std::numeric_limits<float>::quiet_NaN() };
   float gpu_temp_c{ std::numeric_limits<float>::quiet_NaN() };
};

/**
 * A data-race-free seqlock-style publication object. Each word is atomic so a
 * reader retrying around the generation counter remains valid C++, rather than
 * relying on a conventional seqlock's undefined concurrent plain loads.
 */
template <typename T>
class SnapshotPublication
{
   static_assert(std::is_trivially_copyable<T>::value, "Snapshots must be trivially copyable");
   static constexpr size_t word_count = (sizeof(T) + sizeof(uint64_t) - 1) / sizeof(uint64_t);

public:
   void publish(const T &value) noexcept
   {
      generation.fetch_add(1, std::memory_order_acq_rel);
      const auto *source = reinterpret_cast<const unsigned char *>(&value);
      for (size_t i = 0; i < word_count; ++i)
      {
         uint64_t word = 0;
         const size_t offset = i * sizeof(uint64_t);
         const size_t bytes = offset < sizeof(T) ? std::min(sizeof(uint64_t), sizeof(T) - offset) : 0;
         if (bytes != 0)
         {
            std::memcpy(&word, source + offset, bytes);
         }
         words[i].store(word, std::memory_order_relaxed);
      }
      generation.fetch_add(1, std::memory_order_release);
   }

   T read(uint64_t *published_generation = nullptr) const noexcept
   {
      T result{};
      auto *destination = reinterpret_cast<unsigned char *>(&result);
      for (;;)
      {
         const uint64_t before = generation.load(std::memory_order_acquire);
         if ((before & 1u) != 0)
         {
            continue;
         }

         for (size_t i = 0; i < word_count; ++i)
         {
            const uint64_t word = words[i].load(std::memory_order_relaxed);
            const size_t offset = i * sizeof(uint64_t);
            const size_t bytes = offset < sizeof(T) ? std::min(sizeof(uint64_t), sizeof(T) - offset) : 0;
            if (bytes != 0)
            {
               std::memcpy(destination + offset, &word, bytes);
            }
         }

         const uint64_t after = generation.load(std::memory_order_acquire);
         if (before == after)
         {
            if (published_generation != nullptr)
            {
               *published_generation = after;
            }
            return result;
         }
      }
   }

private:
   mutable std::atomic<uint64_t> generation{ 0 };
   std::array<std::atomic<uint64_t>, word_count> words{};
};

class FrameTracker
{
public:
   void present() noexcept
   {
      present_count.fetch_add(1, std::memory_order_relaxed);
   }

   void reset() noexcept
   {
      reset_generation.fetch_add(1, std::memory_order_acq_rel);
      present_count.store(0, std::memory_order_relaxed);
      reset_generation.fetch_add(1, std::memory_order_release);
   }

   void sample(MetricsSnapshot &snapshot, std::chrono::steady_clock::time_point now) noexcept
   {
      uint64_t generation = 0;
      uint64_t current = 0;
      for (;;)
      {
         generation = reset_generation.load(std::memory_order_acquire);
         if ((generation & 1u) != 0)
            continue;
         current = present_count.load(std::memory_order_relaxed);
         if (generation == reset_generation.load(std::memory_order_acquire))
            break;
      }
      if (generation != last_reset_generation)
      {
         last_reset_generation = generation;
         last_count = current;
         last_time = now;
         return;
      }
      if (last_time.time_since_epoch().count() != 0)
      {
         const double elapsed = std::chrono::duration<double>(now - last_time).count();
         const uint64_t frames = current >= last_count ? current - last_count : 0;
         if (elapsed > 0.0)
         {
            snapshot.fps = static_cast<float>(static_cast<double>(frames) / elapsed);
            if (frames != 0)
            {
               snapshot.frame_time_ms = static_cast<float>(elapsed * 1000.0 / static_cast<double>(frames));
            }
         }
      }
      last_count = current;
      last_time = now;
   }

private:
   std::atomic<uint64_t> present_count{ 0 };
   std::atomic<uint64_t> reset_generation{ 0 };
   uint64_t last_reset_generation{ 0 };
   uint64_t last_count{ 0 };
   std::chrono::steady_clock::time_point last_time{};
};

} // namespace mali_wrapper::hud
