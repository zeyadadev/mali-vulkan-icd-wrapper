/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "hud/hud_types.hpp"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace mali_wrapper::hud
{

struct CpuTimes
{
   uint64_t busy{ 0 };
   uint64_t total{ 0 };
};

struct ProcessTimes
{
   uint64_t ticks{ 0 };
};

struct MemoryInfo
{
   uint64_t total_kib{ 0 };
   uint64_t available_kib{ 0 };
};

bool parse_cpu_stat(std::string_view text, CpuTimes &times) noexcept;
bool parse_process_stat(std::string_view text, ProcessTimes &times) noexcept;
bool parse_meminfo(std::string_view text, MemoryInfo &memory) noexcept;
bool parse_process_rss(std::string_view text, uint64_t &rss_kib) noexcept;
bool parse_integer_metric(std::string_view text, int64_t &value) noexcept;
bool parse_mali_load(std::string_view text, float &utilization, uint64_t &frequency_hz) noexcept;

class MetricSampler
{
public:
   MetricSampler();
   ~MetricSampler();
   MetricSampler(const MetricSampler &) = delete;
   MetricSampler &operator=(const MetricSampler &) = delete;

   bool start(uint32_t interval_ms, FrameTracker &frame_tracker, bool force_sensor_failure = false) noexcept;
   void stop() noexcept;
   MetricsSnapshot snapshot(uint64_t *generation = nullptr) const noexcept
   {
      return publication.read(generation);
   }

private:
   void run() noexcept;
   void sample_once() noexcept;

   struct Sources;
   std::unique_ptr<Sources> sources;
   SnapshotPublication<MetricsSnapshot> publication;
   FrameTracker *frames{ nullptr };
   uint32_t interval{ 500 };
   bool sensor_failure{ false };
   std::atomic<bool> stopping{ false };
   std::thread thread;
   std::mutex wait_mutex;
   std::condition_variable wait_condition;
   CpuTimes previous_cpu{};
   ProcessTimes previous_process{};
   std::chrono::steady_clock::time_point previous_process_time{};
   long clock_ticks_per_second{ 100 };
};

} // namespace mali_wrapper::hud
