/*
 * SPDX-License-Identifier: MIT
 */

#include "hud/hud_config.hpp"
#include "hud/hud_layout.hpp"
#include "hud/hud_metadata.hpp"
#include "hud/hud_metrics.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace mali_wrapper::hud;

static void test_config()
{
   HudPosition position = HudPosition::bottom_right;
   assert(parse_position("top-left", position) && position == HudPosition::top_left);
   assert(!parse_position("middle", position));

   bool automatic = false;
   float scale = 0.0f;
   assert(parse_scale("auto", automatic, scale) && automatic);
   assert(parse_scale("1.25", automatic, scale) && !automatic && std::abs(scale - 1.25f) < 0.001f);
   assert(!parse_scale("0.5", automatic, scale));
   assert(!parse_scale("nan", automatic, scale));

   float opacity = 0.55f;
   assert(parse_opacity("0", opacity) && opacity == 0.0f);
   assert(parse_opacity("1", opacity) && opacity == 1.0f);
   assert(!parse_opacity("1.1", opacity));
   assert(!parse_opacity("nan", opacity));

   uint32_t interval = 500;
   assert(parse_interval("100", interval) && interval == 100);
   assert(parse_interval("5000", interval) && interval == 5000);
   assert(!parse_interval("99", interval));
   assert(!parse_interval("5001", interval));

   ::unsetenv("MALI_HUD");
   assert(!load_config().enabled);
   ::setenv("MALI_HUD", "1", 1);
   ::setenv("MALI_HUD_OPACITY", "0.40", 1);
   ::setenv("MALI_HUD_TEXT_OPACITY", "0.85", 1);
   const HudConfig configured = load_config();
   assert(configured.enabled);
   assert(std::abs(configured.opacity - 0.40f) < 0.001f);
   assert(std::abs(configured.text_opacity - 0.85f) < 0.001f);
   ::setenv("MALI_HUD_OPACITY", "invalid", 1);
   ::setenv("MALI_HUD_TEXT_OPACITY", "2.0", 1);
   const HudConfig fallback = load_config();
   assert(std::abs(fallback.opacity - 0.55f) < 0.001f);
   assert(std::abs(fallback.text_opacity - 0.90f) < 0.001f);
   ::unsetenv("MALI_HUD");
   ::unsetenv("MALI_HUD_OPACITY");
   ::unsetenv("MALI_HUD_TEXT_OPACITY");
}

static void test_proc_parsers()
{
   CpuTimes cpu{};
   assert(parse_cpu_stat("cpu  100 20 30 400 10 5 6 7 0 0\ncpu0 1 2 3 4\n", cpu));
   assert(cpu.total == 578);
   assert(cpu.busy == 168);
   assert(!parse_cpu_stat("cpu nope\n", cpu));

   ProcessTimes process{};
   assert(parse_process_stat("123 (game process) R 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15", process));
   assert(process.ticks == 23);
   assert(!parse_process_stat("invalid", process));

   MemoryInfo memory{};
   assert(parse_meminfo(
      "MemTotal:       32768000 kB\nMemFree:         123456 kB\n"
      "MemAvailable:   12345678 kB\n",
      memory));
   assert(memory.total_kib == 32768000);
   assert(memory.available_kib == 12345678);
   assert(!parse_meminfo("MemTotal: 1000 kB\n", memory));
   assert(!parse_meminfo("MemTotal: 1000 kB\nMemAvailable: 1001 kB\n",
                         memory));

   uint64_t rss_kib = 0;
   assert(parse_process_rss("Name:\tgame\nVmRSS:\t  123456 kB\n", rss_kib));
   assert(rss_kib == 123456);
   assert(!parse_process_rss("VmRSS: unknown kB\n", rss_kib));
   assert(!parse_process_rss("VmRSS: 123456\n", rss_kib));

   int64_t integer = 0;
   assert(parse_integer_metric("  47890\n", integer) && integer == 47890);
   assert(!parse_integer_metric("47890 broken", integer));
   float utilization = 0.0f;
   uint64_t frequency = 0;
   assert(parse_mali_load("5@300000000Hz\n", utilization, frequency));
   assert(utilization == 5.0f && frequency == 300000000);
}

static void test_frame_tracking()
{
   FrameTracker tracker;
   MetricsSnapshot sample{};
   const auto start = std::chrono::steady_clock::time_point(std::chrono::seconds(10));
   for (int i = 0; i < 12; ++i)
      tracker.present();
   tracker.sample(sample, start);
   for (int i = 0; i < 60; ++i)
      tracker.present();
   tracker.sample(sample, start + std::chrono::seconds(1));
   assert(std::abs(sample.fps - 60.0f) < 0.001f);
   assert(std::abs(sample.frame_time_ms - (1000.0f / 60.0f)) < 0.001f);

   tracker.reset();
   sample = {};
   tracker.sample(sample, start + std::chrono::seconds(2));
   for (int i = 0; i < 30; ++i)
      tracker.present();
   tracker.sample(sample, start + std::chrono::seconds(3));
   assert(std::abs(sample.fps - 30.0f) < 0.001f);
}

static void test_panel_layout()
{
   HudConfig config{};
   config.automatic_scale = false;

   for (float scale : { 0.75f, 1.25f, 2.0f, 3.0f })
   {
      config.scale = scale;
      for (uint32_t height : { 480u, 720u, 1080u, 2160u })
      {
         for (HudPosition position :
              { HudPosition::top_left, HudPosition::top_right,
                HudPosition::bottom_left, HudPosition::bottom_right })
         {
            config.position = position;
            const HudPanelLayout layout =
               calculate_panel_layout(3840, height, config);
            assert(layout.height <= height / 2);
            assert(layout.x >= 0 && layout.y >= 0);
            assert(static_cast<uint64_t>(layout.x) + layout.width <= 3840);
            assert(static_cast<uint64_t>(layout.y) + layout.height <= height);
            assert(layout.scale <= scale);
         }
      }
   }

   config.scale = 1.25f;
   const HudPanelLayout screenshot =
      calculate_panel_layout(1280, 720, config);
   assert(screenshot.height == 210);
   assert(std::abs(screenshot.scale - 1.25f) < 0.001f);
   assert(HUD_LINE_FONT_HEIGHTS[3] > HUD_LINE_FONT_HEIGHTS[0]);
   assert(HUD_LINE_FONT_HEIGHTS[4] > HUD_LINE_FONT_HEIGHTS[8]);
   for (size_t line = 1; line < HUD_LINE_COUNT; ++line)
      assert(HUD_LINE_BASELINES[line] > HUD_LINE_BASELINES[line - 1]);

   config.scale = 3.0f;
   const HudPanelLayout capped = calculate_panel_layout(1280, 720, config);
   assert(capped.height == 360);
   assert(capped.scale < 3.0f);

   config.position = HudPosition::top_left;
   const HudPanelLayout left = calculate_panel_layout(1280, 720, config);
   const HudPanelLayout fitted_left =
      fit_panel_to_content(left, 600, config.position);
   assert(fitted_left.x == left.x);
   assert(fitted_left.width == 600);

   config.position = HudPosition::top_right;
   const HudPanelLayout right = calculate_panel_layout(1280, 720, config);
   const HudPanelLayout fitted_right =
      fit_panel_to_content(right, 600, config.position);
   assert(fitted_right.x + static_cast<int32_t>(fitted_right.width) ==
          right.x + static_cast<int32_t>(right.width));
   assert(fitted_right.width == 600);
   assert(fit_panel_to_content(right, right.width + 100, config.position).width ==
          right.width);
}

static void test_metadata()
{
   assert(format_vk_version(VK_MAKE_VERSION(1, 3, 276)) == "1.3.276");
   assert(format_driver_version(0x13B5, VK_MAKE_VERSION(42, 7, 9)) == "42.7.9");

   VkApplicationInfo dxvk_info{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
   dxvk_info.pApplicationName = "game.exe";
   dxvk_info.applicationVersion = 1;
   dxvk_info.pEngineName = "DXVK";
   dxvk_info.engineVersion = VK_MAKE_API_VERSION(0, 2, 7, 1);
   dxvk_info.apiVersion = VK_API_VERSION_1_3;
   const ApplicationMetadata captured = copy_application_metadata(&dxvk_info);
   assert(captured.stack == ApiStackKind::dxvk);
   assert(captured.dxvk_client == DxvkClientApi::d3d9);
   assert(std::strcmp(captured.translation_version.data(), "2.7.1") == 0);
   assert(classify_api_stack(captured) == "D3D9 -> DXVK 2.7.1 -> Vulkan");

   std::array<char, HUD_TEXT_SHORT> embedded_version{};
   const char dxvk_binary[] = "unrelated v13.0.0\0DXVK\0build\0v2.7.1-4-gdeadbeef\0";
   assert(extract_dxvk_version(std::string_view(dxvk_binary, sizeof(dxvk_binary)),
                               embedded_version));
   assert(std::strcmp(embedded_version.data(), "2.7.1-4-gdeadbeef") == 0);
   embedded_version = {};
   const char wine_binary[] = "Wine Direct3D\0v10.0.0\0";
   assert(!extract_dxvk_version(std::string_view(wine_binary, sizeof(wine_binary)),
                                embedded_version));
   const char mesa_binary[] =
      "Mesa Gallium driver 25.2.8-0ubuntu0.24.04.2 for %s\0LLVM 20.1.2\0";
   assert(extract_mesa_version(std::string_view(mesa_binary, sizeof(mesa_binary)),
                               embedded_version));
   assert(std::strcmp(embedded_version.data(), "25.2.8") == 0);

   char temporary_directory[] = "/tmp/mali-hud-dxvk-XXXXXX";
   assert(::mkdtemp(temporary_directory) != nullptr);
   char mapped_path[PATH_MAX]{};
   assert(std::snprintf(mapped_path, sizeof(mapped_path), "%s/d3d9.dll",
                        temporary_directory) > 0);
   const int mapped_fd = ::open(mapped_path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
   assert(mapped_fd >= 0);
   const char mapped_dxvk[] = "portable executable\0DXVK\0release\0v2.6.2\0";
   assert(::write(mapped_fd, mapped_dxvk, sizeof(mapped_dxvk)) ==
          static_cast<ssize_t>(sizeof(mapped_dxvk)));
   void *mapped = ::mmap(nullptr, sizeof(mapped_dxvk), PROT_READ, MAP_PRIVATE, mapped_fd, 0);
   assert(mapped != MAP_FAILED);
   const ApplicationMetadata discovered = copy_application_metadata(nullptr);
   assert(discovered.stack == ApiStackKind::dxvk);
   assert(discovered.dxvk_client == DxvkClientApi::d3d9);
   assert(discovered.stack_from_module_scan);
   assert(std::strcmp(discovered.translation_version.data(), "2.6.2") == 0);
   assert(application_metadata_priority(discovered) == 80);
   VkApplicationInfo legacy_dxvk_info{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
   legacy_dxvk_info.pEngineName = "DXVK";
   legacy_dxvk_info.engineVersion = VK_MAKE_VERSION(1, 11, 0);
   const ApplicationMetadata legacy_dxvk = copy_application_metadata(&legacy_dxvk_info);
   assert(legacy_dxvk.dxvk_client == DxvkClientApi::d3d9);
   assert(!legacy_dxvk.stack_from_module_scan);
   assert(std::strcmp(legacy_dxvk.translation_version.data(), "1.11.0") == 0);
   assert(application_metadata_priority(legacy_dxvk) == 100);
   assert(::munmap(mapped, sizeof(mapped_dxvk)) == 0);
   ::close(mapped_fd);
   assert(::unlink(mapped_path) == 0);

   assert(std::snprintf(mapped_path, sizeof(mapped_path), "%s/d3d11.dll",
                        temporary_directory) > 0);
   const int d3d11_fd = ::open(mapped_path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
   assert(d3d11_fd >= 0);
   const char mapped_d3d11[] = "portable executable\0DXVK\0release\0v3.0.0\0";
   assert(::write(d3d11_fd, mapped_d3d11, sizeof(mapped_d3d11)) ==
          static_cast<ssize_t>(sizeof(mapped_d3d11)));
   void *d3d11_mapping =
      ::mmap(nullptr, sizeof(mapped_d3d11), PROT_READ, MAP_PRIVATE, d3d11_fd, 0);
   assert(d3d11_mapping != MAP_FAILED);
   VkApplicationInfo d3d11_info{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
   d3d11_info.pEngineName = "DXVK";
   d3d11_info.engineVersion = VK_MAKE_VERSION(3, 0, 0);
   const ApplicationMetadata d3d11 = copy_application_metadata(&d3d11_info);
   assert(d3d11.dxvk_client == DxvkClientApi::d3d11);
   assert(classify_api_stack(d3d11) == "D3D11 -> DXVK 3.0.0 -> Vulkan");
   assert(::munmap(d3d11_mapping, sizeof(mapped_d3d11)) == 0);
   ::close(d3d11_fd);
   assert(::unlink(mapped_path) == 0);
   assert(::rmdir(temporary_directory) == 0);

   ApplicationMetadata metadata{};
   std::strcpy(metadata.engine_name.data(), "DXVK");
   metadata.engine_version = VK_MAKE_VERSION(3, 0, 2);
   metadata.stack = ApiStackKind::dxvk;
   std::strcpy(metadata.translation_version.data(), "3.0.2");
   assert(classify_api_stack(metadata) == "DXVK 3.0.2 -> Vulkan");
   metadata.application_version = 1;
   metadata.dxvk_client = DxvkClientApi::d3d9;
   assert(classify_api_stack(metadata) == "D3D9 -> DXVK 3.0.2 -> Vulkan");
   assert(application_metadata_priority(metadata) == 100);
   metadata.dxvk_client = DxvkClientApi::d3d11;
   assert(classify_api_stack(metadata) == "D3D11 -> DXVK 3.0.2 -> Vulkan");

   metadata = {};
   std::strcpy(metadata.engine_name.data(), "mesa zink");
   metadata.stack = ApiStackKind::zink;
   assert(classify_api_stack(metadata) == "OpenGL -> Zink -> Vulkan");
   std::strcpy(metadata.translation_version.data(), "25.2.8");
   assert(classify_api_stack(metadata) == "OpenGL -> Zink 25.2.8 -> Vulkan");

   metadata = {};
   std::strcpy(metadata.engine_name.data(), "vkd3d");
   metadata.stack = ApiStackKind::vkd3d;
   metadata.engine_version = VK_MAKE_VERSION(2, 13, 1);
   assert(classify_api_stack(metadata) == "D3D12 -> VKD3D-Proton 2.13.1 -> Vulkan");

   metadata = {};
   metadata.api_version = VK_API_VERSION_1_3;
   assert(application_metadata_priority(metadata) == 10);
   std::strcpy(metadata.application_name.data(), "probe");
   assert(application_metadata_priority(metadata) == 20);
   std::strcpy(metadata.engine_name.data(), "custom");
   assert(application_metadata_priority(metadata) == 30);
}

static void test_snapshot_publication()
{
   SnapshotPublication<MetricsSnapshot> publication;
   std::atomic<bool> done{ false };
   std::thread writer([&] {
      for (uint32_t i = 1; i < 100000; ++i)
      {
         MetricsSnapshot value{};
         value.monotonic_ns = i;
         value.fps = static_cast<float>(i);
         value.frame_time_ms = static_cast<float>(i * 2);
         value.system_ram_used_gib = static_cast<float>(i * 3);
         value.process_ram_mib = static_cast<float>(i * 4);
         publication.publish(value);
      }
      done.store(true, std::memory_order_release);
   });

   std::vector<std::thread> readers;
   for (int reader = 0; reader < 4; ++reader)
   {
      readers.emplace_back([&] {
         while (!done.load(std::memory_order_acquire))
         {
            const MetricsSnapshot value = publication.read();
            if (value.monotonic_ns != 0)
            {
               assert(value.fps == static_cast<float>(value.monotonic_ns));
               assert(value.frame_time_ms == static_cast<float>(value.monotonic_ns * 2));
               assert(value.system_ram_used_gib ==
                      static_cast<float>(value.monotonic_ns * 3));
               assert(value.process_ram_mib ==
                      static_cast<float>(value.monotonic_ns * 4));
            }
         }
      });
   }

   writer.join();
   for (auto &reader : readers)
      reader.join();
}

int main()
{
   test_config();
   test_proc_parsers();
   test_frame_tracking();
   test_panel_layout();
   test_metadata();
   test_snapshot_publication();
   std::cout << "HUD unit tests passed\n";
   return 0;
}
