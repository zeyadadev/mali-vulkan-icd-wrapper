/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "hud/hud_types.hpp"

#include <string>
#include <string_view>

#include <vulkan/vulkan.h>

namespace mali_wrapper::hud
{

ApplicationMetadata copy_application_metadata(const VkApplicationInfo *application_info) noexcept;
int application_metadata_priority(const ApplicationMetadata &metadata) noexcept;
bool extract_dxvk_version(std::string_view binary,
                          std::array<char, HUD_TEXT_SHORT> &version) noexcept;
bool extract_mesa_version(std::string_view binary,
                          std::array<char, HUD_TEXT_SHORT> &version) noexcept;
std::string classify_api_stack(const ApplicationMetadata &metadata);
std::string format_vk_version(uint32_t version);
std::string format_driver_version(uint32_t vendor_id, uint32_t version);

} // namespace mali_wrapper::hud
