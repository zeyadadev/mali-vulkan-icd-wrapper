/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "hud/hud_types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace mali_wrapper::hud
{

constexpr size_t HUD_LINE_COUNT = 9;
constexpr float HUD_BASE_PANEL_WIDTH = 840.0f;
constexpr float HUD_BASE_PANEL_HEIGHT = 168.0f;
constexpr std::array<float, HUD_LINE_COUNT> HUD_LINE_FONT_HEIGHTS{
   14.0f, 14.0f, 14.0f, 18.0f, 16.0f, 16.0f, 16.0f, 14.0f, 14.0f
};
constexpr std::array<float, HUD_LINE_COUNT> HUD_LINE_BASELINES{
   18.0f, 34.0f, 50.0f, 70.0f, 88.0f, 106.0f, 124.0f, 141.0f, 157.0f
};

struct HudPanelLayout
{
   int32_t x{ 0 };
   int32_t y{ 0 };
   uint32_t width{ 0 };
   uint32_t height{ 0 };
   float scale{ 1.0f };
};

inline HudPanelLayout calculate_panel_layout(uint32_t screen_width,
                                             uint32_t screen_height,
                                             const HudConfig &config) noexcept
{
   HudPanelLayout result{};
   const float requested_scale =
      config.automatic_scale
         ? std::clamp(static_cast<float>(screen_height) / 1080.0f, 0.75f, 3.0f)
         : config.scale;
   const uint32_t maximum_panel_height = std::max(1u, screen_height / 2u);
   const float vertical_scale_limit =
      static_cast<float>(maximum_panel_height) / HUD_BASE_PANEL_HEIGHT;
   result.scale = std::min(requested_scale, vertical_scale_limit);

   const uint32_t margin =
      std::max(4u, static_cast<uint32_t>(std::lround(12.0f * result.scale)));
   const uint32_t available_width =
      screen_width > margin * 2 ? screen_width - margin * 2 : screen_width;
   const uint32_t available_height =
      screen_height > margin * 2 ? screen_height - margin * 2 : screen_height;
   result.width = std::min(
      available_width,
      std::max(1u, static_cast<uint32_t>(
                      std::lround(HUD_BASE_PANEL_WIDTH * result.scale))));
   result.height = std::min(
      { available_height, maximum_panel_height,
        std::max(1u, static_cast<uint32_t>(
                        std::lround(HUD_BASE_PANEL_HEIGHT * result.scale))) });

   const bool right =
      config.position == HudPosition::top_right ||
      config.position == HudPosition::bottom_right;
   const bool bottom =
      config.position == HudPosition::bottom_left ||
      config.position == HudPosition::bottom_right;
   const int64_t right_x =
      static_cast<int64_t>(screen_width) - result.width - margin;
   const int64_t bottom_y =
      static_cast<int64_t>(screen_height) - result.height - margin;
   result.x = right ? static_cast<int32_t>(std::max<int64_t>(0, right_x))
                    : static_cast<int32_t>(margin);
   result.y = bottom ? static_cast<int32_t>(std::max<int64_t>(0, bottom_y))
                     : static_cast<int32_t>(margin);
   return result;
}

inline HudPanelLayout fit_panel_to_content(const HudPanelLayout &maximum,
                                           uint32_t content_width,
                                           HudPosition position) noexcept
{
   HudPanelLayout result = maximum;
   result.width = std::min(maximum.width, std::max(1u, content_width));
   if (position == HudPosition::top_right ||
       position == HudPosition::bottom_right)
   {
      result.x += static_cast<int32_t>(maximum.width - result.width);
   }
   return result;
}

} // namespace mali_wrapper::hud
