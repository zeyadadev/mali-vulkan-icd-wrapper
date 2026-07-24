/*
 * SPDX-License-Identifier: MIT
 */

#include "hud/hud_metrics.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace mali_wrapper::hud
{
namespace
{

class FileDescriptor
{
public:
   FileDescriptor() = default;
   explicit FileDescriptor(const std::filesystem::path &path)
      : value(::open(path.c_str(), O_RDONLY | O_CLOEXEC))
   {
   }
   ~FileDescriptor()
   {
      if (value >= 0)
      {
         ::close(value);
      }
   }
   FileDescriptor(const FileDescriptor &) = delete;
   FileDescriptor &operator=(const FileDescriptor &) = delete;
   FileDescriptor(FileDescriptor &&other) noexcept
      : value(other.value)
   {
      other.value = -1;
   }
   FileDescriptor &operator=(FileDescriptor &&other) noexcept
   {
      std::swap(value, other.value);
      return *this;
   }
   bool valid() const noexcept
   {
      return value >= 0;
   }
   int get() const noexcept
   {
      return value;
   }

private:
   int value{ -1 };
};

ssize_t read_descriptor(int fd, char *buffer, size_t capacity) noexcept
{
   if (fd < 0 || capacity < 2 || lseek(fd, 0, SEEK_SET) < 0)
   {
      return -1;
   }
   const ssize_t length = ::read(fd, buffer, capacity - 1);
   if (length < 0)
   {
      return -1;
   }
   buffer[length] = '\0';
   return length;
}

std::string read_small_file(const std::filesystem::path &path)
{
   FileDescriptor descriptor(path);
   char buffer[256]{};
   const ssize_t length = descriptor.valid() ? read_descriptor(descriptor.get(), buffer, sizeof(buffer)) : -1;
   return length > 0 ? std::string(buffer, static_cast<size_t>(length)) : std::string{};
}

bool starts_with(std::string_view value, std::string_view prefix) noexcept
{
   return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool parse_unsigned(std::string_view value, uint64_t &result) noexcept
{
   while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\n'))
   {
      value.remove_prefix(1);
   }
   size_t length = 0;
   while (length < value.size() && value[length] >= '0' && value[length] <= '9')
   {
      ++length;
   }
   if (length == 0)
   {
      return false;
   }
   const char *begin = value.data();
   const char *end = begin + length;
   auto parsed = std::from_chars(begin, end, result);
   return parsed.ec == std::errc{} && parsed.ptr == end;
}

float unavailable() noexcept
{
   return std::numeric_limits<float>::quiet_NaN();
}

bool parse_kib_field(std::string_view text, std::string_view name, uint64_t &value) noexcept
{
   while (!text.empty())
   {
      const size_t line_end = text.find('\n');
      std::string_view line = text.substr(0, line_end);
      if (starts_with(line, name))
      {
         line.remove_prefix(name.size());
         while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);

         const char *begin = line.data();
         const char *end = begin + line.size();
         auto parsed = std::from_chars(begin, end, value);
         if (parsed.ec != std::errc{})
            return false;
         while (parsed.ptr != end && (*parsed.ptr == ' ' || *parsed.ptr == '\t'))
            ++parsed.ptr;
         if (end - parsed.ptr < 2 || parsed.ptr[0] != 'k' || parsed.ptr[1] != 'B')
            return false;
         parsed.ptr += 2;
         while (parsed.ptr != end &&
                (*parsed.ptr == ' ' || *parsed.ptr == '\t' || *parsed.ptr == '\r'))
         {
            ++parsed.ptr;
         }
         return parsed.ptr == end;
      }
      if (line_end == std::string_view::npos)
         break;
      text.remove_prefix(line_end + 1);
   }
   return false;
}

} // namespace

struct MetricSampler::Sources
{
   FileDescriptor proc_stat{ "/proc/stat" };
   FileDescriptor process_stat{ "/proc/self/stat" };
   FileDescriptor meminfo{ "/proc/meminfo" };
   FileDescriptor process_status{ "/proc/self/status" };
   std::vector<FileDescriptor> cpu_temperatures;
   FileDescriptor gpu_utilization;
   FileDescriptor gpu_frequency;
   FileDescriptor gpu_load;
   FileDescriptor gpu_temperature;

   Sources()
   {
      std::error_code error;
      const std::filesystem::path thermal_root("/sys/class/thermal");
      if (std::filesystem::exists(thermal_root, error))
      {
         for (const auto &entry : std::filesystem::directory_iterator(thermal_root, error))
         {
            if (error)
               break;
            const std::string type = read_small_file(entry.path() / "type");
            const bool cpu = starts_with(type, "bigcore") || starts_with(type, "littlecore");
            const bool gpu = starts_with(type, "gpu-thermal") || type == "gpu\n" || type == "gpu";
            if (cpu)
            {
               FileDescriptor fd(entry.path() / "temp");
               if (fd.valid())
                  cpu_temperatures.emplace_back(std::move(fd));
            }
            if (gpu && !gpu_temperature.valid())
            {
               gpu_temperature = FileDescriptor(entry.path() / "temp");
            }
         }
      }

      const std::filesystem::path devfreq_root("/sys/class/devfreq");
      error.clear();
      if (std::filesystem::exists(devfreq_root, error))
      {
         for (const auto &entry : std::filesystem::directory_iterator(devfreq_root, error))
         {
            if (error)
               break;
            const std::string gpuinfo = read_small_file(entry.path() / "device/gpuinfo");
            if (gpuinfo.find("Mali") == std::string::npos && gpuinfo.find("mali") == std::string::npos)
               continue;

            gpu_utilization = FileDescriptor(entry.path() / "device/utilisation");
            gpu_frequency = FileDescriptor(entry.path() / "cur_freq");
            gpu_load = FileDescriptor(entry.path() / "load");
            break;
         }
      }
   }
};

bool parse_cpu_stat(std::string_view text, CpuTimes &times) noexcept
{
   const size_t line_end = text.find('\n');
   if (line_end != std::string_view::npos)
   {
      text = text.substr(0, line_end);
   }
   if (!starts_with(text, "cpu "))
   {
      return false;
   }
   text.remove_prefix(4);

   uint64_t values[10]{};
   size_t count = 0;
   while (!text.empty() && count < std::size(values))
   {
      while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
         text.remove_prefix(1);
      if (text.empty())
         break;
      size_t end = text.find_first_of(" \t");
      const std::string_view token = text.substr(0, end);
      auto parsed = std::from_chars(token.data(), token.data() + token.size(), values[count]);
      if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size())
         return false;
      ++count;
      if (end == std::string_view::npos)
         break;
      text.remove_prefix(end);
   }
   if (count < 4)
   {
      return false;
   }

   uint64_t total = 0;
   for (size_t i = 0; i < count; ++i)
      total += values[i];
   const uint64_t idle = values[3] + (count > 4 ? values[4] : 0);
   times.total = total;
   times.busy = total >= idle ? total - idle : 0;
   return true;
}

bool parse_process_stat(std::string_view text, ProcessTimes &times) noexcept
{
   const size_t command_end = text.rfind(')');
   if (command_end == std::string_view::npos || command_end + 2 >= text.size())
   {
      return false;
   }
   text.remove_prefix(command_end + 1);

   uint64_t user_ticks = 0;
   uint64_t system_ticks = 0;
   uint32_t field = 3;
   while (!text.empty() && field <= 15)
   {
      while (!text.empty() && text.front() == ' ')
         text.remove_prefix(1);
      if (text.empty())
         return false;
      const size_t end = text.find(' ');
      const std::string_view token = text.substr(0, end);
      if (field == 14 || field == 15)
      {
         uint64_t value = 0;
         auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
         if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size())
            return false;
         if (field == 14)
            user_ticks = value;
         else
            system_ticks = value;
      }
      ++field;
      if (end == std::string_view::npos)
         break;
      text.remove_prefix(end);
   }
   if (field <= 15)
   {
      return false;
   }
   times.ticks = user_ticks + system_ticks;
   return true;
}

bool parse_meminfo(std::string_view text, MemoryInfo &memory) noexcept
{
   MemoryInfo parsed{};
   if (!parse_kib_field(text, "MemTotal:", parsed.total_kib) ||
       !parse_kib_field(text, "MemAvailable:", parsed.available_kib) ||
       parsed.total_kib == 0 || parsed.available_kib > parsed.total_kib)
   {
      return false;
   }
   memory = parsed;
   return true;
}

bool parse_process_rss(std::string_view text, uint64_t &rss_kib) noexcept
{
   return parse_kib_field(text, "VmRSS:", rss_kib);
}

bool parse_integer_metric(std::string_view text, int64_t &value) noexcept
{
   while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n'))
      text.remove_prefix(1);
   if (text.empty())
      return false;
   const char *begin = text.data();
   const char *end = begin + text.size();
   auto parsed = std::from_chars(begin, end, value);
   if (parsed.ec != std::errc{})
      return false;
   while (parsed.ptr != end &&
          (*parsed.ptr == ' ' || *parsed.ptr == '\t' || *parsed.ptr == '\n' || *parsed.ptr == '\r'))
   {
      ++parsed.ptr;
   }
   return parsed.ptr == end;
}

bool parse_mali_load(std::string_view text, float &utilization, uint64_t &frequency_hz) noexcept
{
   const size_t separator = text.find('@');
   const size_t suffix = text.find("Hz", separator == std::string_view::npos ? 0 : separator);
   if (separator == std::string_view::npos || suffix == std::string_view::npos)
   {
      return false;
   }
   int64_t load = 0;
   if (!parse_integer_metric(text.substr(0, separator), load) || load < 0)
      return false;
   uint64_t frequency = 0;
   if (!parse_unsigned(text.substr(separator + 1, suffix - separator - 1), frequency))
      return false;
   utilization = static_cast<float>(load);
   frequency_hz = frequency;
   return true;
}

MetricSampler::~MetricSampler()
{
   stop();
}

MetricSampler::MetricSampler() = default;

bool MetricSampler::start(uint32_t interval_ms, FrameTracker &frame_tracker, bool force_sensor_failure) noexcept
{
   if (thread.joinable())
   {
      return true;
   }
   interval = std::clamp(interval_ms, 100u, 5000u);
   frames = &frame_tracker;
   sensor_failure = force_sensor_failure;
   stopping.store(false, std::memory_order_release);
   previous_cpu = {};
   previous_process = {};
   previous_process_time = {};
   clock_ticks_per_second = std::max(1L, sysconf(_SC_CLK_TCK));
   try
   {
      if (!sensor_failure)
      {
         sources = std::make_unique<Sources>();
      }
      thread = std::thread(&MetricSampler::run, this);
   }
   catch (...)
   {
      sources.reset();
      frames = nullptr;
      return false;
   }
   return true;
}

void MetricSampler::stop() noexcept
{
   stopping.store(true, std::memory_order_release);
   wait_condition.notify_all();
   if (thread.joinable())
   {
      thread.join();
   }
   sources.reset();
   frames = nullptr;
}

void MetricSampler::run() noexcept
{
   do
   {
      sample_once();
      std::unique_lock<std::mutex> lock(wait_mutex);
      wait_condition.wait_for(lock, std::chrono::milliseconds(interval),
                              [this] { return stopping.load(std::memory_order_acquire); });
   } while (!stopping.load(std::memory_order_acquire));
}

void MetricSampler::sample_once() noexcept
{
   MetricsSnapshot result{};
   const auto now = std::chrono::steady_clock::now();
   result.monotonic_ns =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
   if (frames != nullptr)
   {
      frames->sample(result, now);
   }

   if (sensor_failure || !sources)
   {
      publication.publish(result);
      return;
   }

   char buffer[4096]{};
   ssize_t length = read_descriptor(sources->proc_stat.get(), buffer, sizeof(buffer));
   CpuTimes current_cpu{};
   if (length > 0 && parse_cpu_stat(std::string_view(buffer, static_cast<size_t>(length)), current_cpu))
   {
      if (previous_cpu.total != 0 && current_cpu.total > previous_cpu.total &&
          current_cpu.busy >= previous_cpu.busy)
      {
         const uint64_t total_delta = current_cpu.total - previous_cpu.total;
         const uint64_t busy_delta = current_cpu.busy - previous_cpu.busy;
         result.system_cpu_percent = static_cast<float>(100.0 * static_cast<double>(busy_delta) /
                                                        static_cast<double>(total_delta));
      }
      previous_cpu = current_cpu;
   }

   length = read_descriptor(sources->process_stat.get(), buffer, sizeof(buffer));
   ProcessTimes current_process{};
   if (length > 0 && parse_process_stat(std::string_view(buffer, static_cast<size_t>(length)), current_process))
   {
      if (previous_process_time.time_since_epoch().count() != 0 && current_process.ticks >= previous_process.ticks)
      {
         const double seconds = std::chrono::duration<double>(now - previous_process_time).count();
         if (seconds > 0.0)
         {
            const double ticks = static_cast<double>(current_process.ticks - previous_process.ticks);
            result.process_cpu_percent =
               static_cast<float>(100.0 * ticks / (static_cast<double>(clock_ticks_per_second) * seconds));
         }
      }
      previous_process = current_process;
      previous_process_time = now;
   }

   length = read_descriptor(sources->meminfo.get(), buffer, sizeof(buffer));
   MemoryInfo memory{};
   if (length > 0 && parse_meminfo(std::string_view(buffer, static_cast<size_t>(length)), memory))
   {
      constexpr double KIB_PER_GIB = 1024.0 * 1024.0;
      result.system_ram_total_gib = static_cast<float>(static_cast<double>(memory.total_kib) / KIB_PER_GIB);
      result.system_ram_used_gib =
         static_cast<float>(static_cast<double>(memory.total_kib - memory.available_kib) / KIB_PER_GIB);
   }

   length = read_descriptor(sources->process_status.get(), buffer, sizeof(buffer));
   uint64_t process_rss_kib = 0;
   if (length > 0 &&
       parse_process_rss(std::string_view(buffer, static_cast<size_t>(length)), process_rss_kib))
   {
      result.process_ram_mib = static_cast<float>(static_cast<double>(process_rss_kib) / 1024.0);
   }

   float hottest_cpu = unavailable();
   for (const FileDescriptor &temperature : sources->cpu_temperatures)
   {
      char value_text[64]{};
      const ssize_t value_length = read_descriptor(temperature.get(), value_text, sizeof(value_text));
      int64_t value = 0;
      if (value_length > 0 && parse_integer_metric(std::string_view(value_text, value_length), value))
      {
         const float celsius = std::abs(value) > 1000 ? static_cast<float>(value) / 1000.0f : static_cast<float>(value);
         hottest_cpu = std::isnan(hottest_cpu) ? celsius : std::max(hottest_cpu, celsius);
      }
   }
   result.cpu_temp_c = hottest_cpu;

   char value_text[128]{};
   int64_t value = 0;
   length = read_descriptor(sources->gpu_utilization.get(), value_text, sizeof(value_text));
   if (length > 0 && parse_integer_metric(std::string_view(value_text, length), value))
   {
      result.gpu_percent = static_cast<float>(value);
   }
   length = read_descriptor(sources->gpu_frequency.get(), value_text, sizeof(value_text));
   if (length > 0 && parse_integer_metric(std::string_view(value_text, length), value))
   {
      result.gpu_clock_mhz = static_cast<float>(value) / 1000000.0f;
   }
   if (std::isnan(result.gpu_percent) || std::isnan(result.gpu_clock_mhz))
   {
      float utilization = unavailable();
      uint64_t frequency = 0;
      length = read_descriptor(sources->gpu_load.get(), value_text, sizeof(value_text));
      if (length > 0 && parse_mali_load(std::string_view(value_text, length), utilization, frequency))
      {
         if (std::isnan(result.gpu_percent))
            result.gpu_percent = utilization;
         if (std::isnan(result.gpu_clock_mhz))
            result.gpu_clock_mhz = static_cast<float>(frequency) / 1000000.0f;
      }
   }
   length = read_descriptor(sources->gpu_temperature.get(), value_text, sizeof(value_text));
   if (length > 0 && parse_integer_metric(std::string_view(value_text, length), value))
   {
      result.gpu_temp_c = std::abs(value) > 1000 ? static_cast<float>(value) / 1000.0f : static_cast<float>(value);
   }

   publication.publish(result);
}

} // namespace mali_wrapper::hud
