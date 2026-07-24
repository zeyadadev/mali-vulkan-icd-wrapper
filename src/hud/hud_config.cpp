/*
 * SPDX-License-Identifier: MIT
 */

#include "hud/hud_config.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace mali_wrapper::hud
{
namespace
{

bool parse_float(std::string_view value, float &result) noexcept
{
   if (value.empty() || value.size() >= 32)
   {
      return false;
   }
   char buffer[32]{};
   std::memcpy(buffer, value.data(), value.size());
   char *end = nullptr;
   errno = 0;
   const float parsed = std::strtof(buffer, &end);
   if (errno != 0 || end != buffer + value.size() || !std::isfinite(parsed))
   {
      return false;
   }
   result = parsed;
   return true;
}

} // namespace

bool parse_enabled(std::string_view value) noexcept
{
   return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool parse_position(std::string_view value, HudPosition &position) noexcept
{
   if (value == "top-left")
   {
      position = HudPosition::top_left;
   }
   else if (value == "top-right")
   {
      position = HudPosition::top_right;
   }
   else if (value == "bottom-left")
   {
      position = HudPosition::bottom_left;
   }
   else if (value == "bottom-right")
   {
      position = HudPosition::bottom_right;
   }
   else
   {
      return false;
   }
   return true;
}

bool parse_scale(std::string_view value, bool &automatic, float &scale) noexcept
{
   if (value == "auto")
   {
      automatic = true;
      scale = 1.0f;
      return true;
   }

   float parsed = 0.0f;
   if (!parse_float(value, parsed) || parsed < 0.75f || parsed > 3.0f)
   {
      return false;
   }
   automatic = false;
   scale = parsed;
   return true;
}

bool parse_opacity(std::string_view value, float &opacity) noexcept
{
   float parsed = 0.0f;
   if (!parse_float(value, parsed) || parsed < 0.0f || parsed > 1.0f)
   {
      return false;
   }
   opacity = parsed;
   return true;
}

bool parse_interval(std::string_view value, uint32_t &interval_ms) noexcept
{
   if (value.empty() || value.size() >= 16)
   {
      return false;
   }
   char buffer[16]{};
   std::memcpy(buffer, value.data(), value.size());
   char *end = nullptr;
   errno = 0;
   const unsigned long parsed = std::strtoul(buffer, &end, 10);
   if (errno != 0 || end != buffer + value.size() || parsed < 100 || parsed > 5000)
   {
      return false;
   }
   interval_ms = static_cast<uint32_t>(parsed);
   return true;
}

HudFault parse_test_fault(std::string_view value) noexcept
{
   if (value == "atlas")
      return HudFault::atlas;
   if (value == "buffer")
      return HudFault::buffer;
   if (value == "pipeline")
      return HudFault::pipeline;
   if (value == "sensor")
      return HudFault::sensor;
   return HudFault::none;
}

HudConfig load_config() noexcept
{
   HudConfig config{};
   const char *enabled = std::getenv("MALI_HUD");
   if (enabled == nullptr || !parse_enabled(enabled))
   {
      return config;
   }
   config.enabled = true;

   if (const char *position = std::getenv("MALI_HUD_POSITION"))
   {
      parse_position(position, config.position);
   }
   if (const char *scale = std::getenv("MALI_HUD_SCALE"))
   {
      parse_scale(scale, config.automatic_scale, config.scale);
   }
   if (const char *opacity = std::getenv("MALI_HUD_OPACITY"))
   {
      parse_opacity(opacity, config.opacity);
   }
   if (const char *text_opacity = std::getenv("MALI_HUD_TEXT_OPACITY"))
   {
      parse_opacity(text_opacity, config.text_opacity);
   }
   if (const char *interval = std::getenv("MALI_HUD_INTERVAL_MS"))
   {
      parse_interval(interval, config.interval_ms);
   }
   if (const char *debug = std::getenv("MALI_HUD_DEBUG"))
   {
      config.debug = parse_enabled(debug);
   }
#if MALI_HUD_TEST_BUILD
   if (const char *fault = std::getenv("MALI_HUD_TEST_FAIL"))
   {
      config.test_fault = parse_test_fault(fault);
   }
#endif
   return config;
}

} // namespace mali_wrapper::hud
