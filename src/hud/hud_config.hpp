/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "hud/hud_types.hpp"

#include <string_view>

namespace mali_wrapper::hud
{

bool parse_enabled(std::string_view value) noexcept;
bool parse_position(std::string_view value, HudPosition &position) noexcept;
bool parse_scale(std::string_view value, bool &automatic, float &scale) noexcept;
bool parse_opacity(std::string_view value, float &opacity) noexcept;
bool parse_interval(std::string_view value, uint32_t &interval_ms) noexcept;
HudFault parse_test_fault(std::string_view value) noexcept;
HudConfig load_config() noexcept;

} // namespace mali_wrapper::hud
