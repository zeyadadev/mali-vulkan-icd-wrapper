/*
 * SPDX-License-Identifier: MIT
 */

#include "hud/hud_metadata.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <string_view>
#include <unistd.h>

namespace mali_wrapper::hud
{
namespace
{

constexpr size_t DXVK_SCAN_CHUNK = 64 * 1024;
constexpr size_t DXVK_VERSION_DISTANCE = 512;
constexpr size_t VERSION_SCAN_CARRY = 512;

template <size_t N>
void copy_text(std::array<char, N> &destination, const char *source) noexcept
{
   if (source == nullptr)
   {
      return;
   }
   std::strncpy(destination.data(), source, N - 1);
   destination[N - 1] = '\0';
}

const char *basename(const char *path) noexcept
{
   if (path == nullptr)
   {
      return "";
   }
   const char *slash = std::strrchr(path, '/');
   return slash == nullptr ? path : slash + 1;
}

bool text_equals(const char *left, const char *right) noexcept
{
   return left != nullptr && right != nullptr && std::strcmp(left, right) == 0;
}

void copy_vk_version(std::array<char, HUD_TEXT_SHORT> &destination, uint32_t version) noexcept
{
   std::snprintf(destination.data(), destination.size(), "%u.%u.%u",
                 VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
}

bool has_dll_basename(const char *path, const char *name) noexcept
{
   const char *file = basename(path);
   const size_t name_length = std::strlen(name);
   return std::strncmp(file, name, name_length) == 0 &&
          (file[name_length] == '\0' || std::strcmp(file + name_length, " (deleted)") == 0);
}

DxvkClientApi dxvk_client_from_path(const char *path) noexcept
{
   if (has_dll_basename(path, "d3d8.dll"))
      return DxvkClientApi::d3d8;
   if (has_dll_basename(path, "d3d9.dll"))
      return DxvkClientApi::d3d9;
   if (has_dll_basename(path, "d3d10.dll") ||
       has_dll_basename(path, "d3d10_1.dll") ||
       has_dll_basename(path, "d3d10core.dll"))
      return DxvkClientApi::d3d10;
   if (has_dll_basename(path, "d3d11.dll"))
      return DxvkClientApi::d3d11;
   return DxvkClientApi::unknown;
}

bool parse_semantic_version(const unsigned char *data, size_t size, size_t offset,
                            std::array<char, HUD_TEXT_SHORT> &version) noexcept
{
   if (offset >= size || data[offset] != 'v')
      return false;

   size_t cursor = offset + 1;
   size_t component_digits = 0;
   size_t dots = 0;
   while (cursor < size && std::isdigit(static_cast<unsigned char>(data[cursor])))
   {
      ++cursor;
      ++component_digits;
   }
   if (component_digits == 0)
      return false;

   while (cursor < size && data[cursor] == '.' && dots < 3)
   {
      ++dots;
      ++cursor;
      component_digits = 0;
      while (cursor < size && std::isdigit(static_cast<unsigned char>(data[cursor])))
      {
         ++cursor;
         ++component_digits;
      }
      if (component_digits == 0)
         return false;
   }
   if (dots == 0)
      return false;

   while (cursor < size && cursor - offset < version.size() - 1)
   {
      const unsigned char character = data[cursor];
      if (!(std::isalnum(character) || character == '.' || character == '-' ||
            character == '+' || character == '_'))
         break;
      ++cursor;
   }

   const size_t length = cursor - (offset + 1);
   if (length == 0 || length >= version.size())
      return false;
   std::memcpy(version.data(), data + offset + 1, length);
   version[length] = '\0';
   return true;
}

bool parse_numeric_version(const unsigned char *data, size_t size, size_t offset,
                           std::array<char, HUD_TEXT_SHORT> &version) noexcept
{
   const size_t start = offset;
   size_t component_digits = 0;
   size_t dots = 0;
   while (offset < size && std::isdigit(static_cast<unsigned char>(data[offset])))
   {
      ++offset;
      ++component_digits;
   }
   if (component_digits == 0)
      return false;

   while (offset < size && data[offset] == '.' && dots < 3)
   {
      ++dots;
      ++offset;
      component_digits = 0;
      while (offset < size && std::isdigit(static_cast<unsigned char>(data[offset])))
      {
         ++offset;
         ++component_digits;
      }
      if (component_digits == 0)
         return false;
   }
   if (dots == 0)
      return false;

   const size_t length = offset - start;
   if (length >= version.size())
      return false;
   std::memcpy(version.data(), data + start, length);
   version[length] = '\0';
   return true;
}

} // namespace

bool extract_dxvk_version(std::string_view binary,
                          std::array<char, HUD_TEXT_SHORT> &version) noexcept
{
   const auto *data = reinterpret_cast<const unsigned char *>(binary.data());
   size_t latest_dxvk = SIZE_MAX;
   for (size_t index = 0; index < binary.size(); ++index)
   {
      if (index + 4 <= binary.size() && std::memcmp(data + index, "DXVK", 4) == 0)
         latest_dxvk = index;
      if (data[index] == 'v' && latest_dxvk != SIZE_MAX && index >= latest_dxvk &&
          index - latest_dxvk <= DXVK_VERSION_DISTANCE &&
          parse_semantic_version(data, binary.size(), index, version))
      {
         return true;
      }
   }
   return false;
}

bool extract_mesa_version(std::string_view binary,
                          std::array<char, HUD_TEXT_SHORT> &version) noexcept
{
   const auto *data = reinterpret_cast<const unsigned char *>(binary.data());
   constexpr char marker[] = "Mesa ";
   for (size_t index = 0; index + sizeof(marker) - 1 < binary.size(); ++index)
   {
      if (std::memcmp(data + index, marker, sizeof(marker) - 1) == 0 &&
          parse_numeric_version(data, binary.size(), index + sizeof(marker) - 1, version))
      {
         return true;
      }
   }
   return false;
}

namespace
{

bool scan_dxvk_binary(const char *path, std::array<char, HUD_TEXT_SHORT> &version) noexcept
{
   const int fd = open(path, O_RDONLY | O_CLOEXEC);
   if (fd < 0)
      return false;

   std::array<unsigned char, DXVK_SCAN_CHUNK + DXVK_VERSION_DISTANCE> buffer{};
   size_t carry = 0;
   bool found = false;
   for (;;)
   {
      const ssize_t bytes_read = read(fd, buffer.data() + carry, DXVK_SCAN_CHUNK);
      if (bytes_read <= 0)
         break;
      const size_t bytes = carry + static_cast<size_t>(bytes_read);
      found = extract_dxvk_version(
         std::string_view(reinterpret_cast<const char *>(buffer.data()), bytes), version);
      if (found)
         break;

      carry = std::min(bytes, DXVK_VERSION_DISTANCE);
      std::memmove(buffer.data(), buffer.data() + bytes - carry, carry);
   }
   close(fd);
   return found;
}

bool scan_mesa_binary(const char *path, std::array<char, HUD_TEXT_SHORT> &version) noexcept
{
   const int fd = open(path, O_RDONLY | O_CLOEXEC);
   if (fd < 0)
      return false;

   std::array<unsigned char, DXVK_SCAN_CHUNK + VERSION_SCAN_CARRY> buffer{};
   size_t carry = 0;
   bool found = false;
   for (;;)
   {
      const ssize_t bytes_read = read(fd, buffer.data() + carry, DXVK_SCAN_CHUNK);
      if (bytes_read <= 0)
         break;
      const size_t bytes = carry + static_cast<size_t>(bytes_read);
      found = extract_mesa_version(
         std::string_view(reinterpret_cast<const char *>(buffer.data()), bytes), version);
      if (found)
         break;

      carry = std::min(bytes, VERSION_SCAN_CARRY);
      std::memmove(buffer.data(), buffer.data() + bytes - carry, carry);
   }
   close(fd);
   return found;
}

bool mesa_version_from_path(const char *path,
                            std::array<char, HUD_TEXT_SHORT> &version) noexcept
{
   const std::string_view file(basename(path));
   constexpr std::string_view prefix = "libgallium-";
   const size_t start = file.find(prefix);
   if (start == std::string_view::npos)
      return false;
   const auto *data = reinterpret_cast<const unsigned char *>(file.data());
   return parse_numeric_version(data, file.size(), start + prefix.size(), version);
}

void discover_mapped_mesa(ApplicationMetadata &metadata) noexcept
{
   FILE *maps = std::fopen("/proc/self/maps", "re");
   if (maps == nullptr)
      return;

   char line[PATH_MAX + 256]{};
   char previous_path[PATH_MAX]{};
   while (std::fgets(line, sizeof(line), maps) != nullptr)
   {
      char *path = std::strchr(line, '/');
      if (path == nullptr)
         continue;
      path[std::strcspn(path, "\r\n")] = '\0';
      if (std::strcmp(path, previous_path) == 0)
         continue;
      std::strncpy(previous_path, path, sizeof(previous_path) - 1);
      previous_path[sizeof(previous_path) - 1] = '\0';

      const char *file = basename(path);
      const bool mesa_module = std::strstr(file, "libgallium") != nullptr ||
                               std::strstr(file, "zink_dri.so") != nullptr ||
                               std::strstr(file, "libGLX_mesa.so") != nullptr;
      if (!mesa_module)
         continue;
      if (mesa_version_from_path(path, metadata.translation_version) ||
          scan_mesa_binary(path, metadata.translation_version))
      {
         break;
      }
   }
   std::fclose(maps);
}

void discover_mapped_dxvk(ApplicationMetadata &metadata) noexcept
{
   FILE *maps = std::fopen("/proc/self/maps", "re");
   if (maps == nullptr)
      return;

   char line[PATH_MAX + 256]{};
   char previous_path[PATH_MAX]{};
   ApplicationMetadata best = metadata;
   while (std::fgets(line, sizeof(line), maps) != nullptr)
   {
      char *path = std::strchr(line, '/');
      if (path == nullptr)
         continue;
      path[std::strcspn(path, "\r\n")] = '\0';
      const char deleted_suffix[] = " (deleted)";
      const size_t path_length = std::strlen(path);
      if (path_length >= sizeof(deleted_suffix) - 1 &&
          std::strcmp(path + path_length - (sizeof(deleted_suffix) - 1), deleted_suffix) == 0)
      {
         path[path_length - (sizeof(deleted_suffix) - 1)] = '\0';
      }
      if (std::strcmp(path, previous_path) == 0)
         continue;
      std::strncpy(previous_path, path, sizeof(previous_path) - 1);
      previous_path[sizeof(previous_path) - 1] = '\0';

      const DxvkClientApi mapped_client = dxvk_client_from_path(path);
      const bool candidate = mapped_client != DxvkClientApi::unknown ||
                             has_dll_basename(path, "dxgi.dll");
      if (!candidate)
         continue;

      std::array<char, HUD_TEXT_SHORT> version{};
      if (!scan_dxvk_binary(path, version))
         continue;

      const bool module_is_only_evidence = metadata.stack == ApiStackKind::native_vulkan;
      best.stack = ApiStackKind::dxvk;
      if (best.dxvk_client == DxvkClientApi::unknown &&
          mapped_client != DxvkClientApi::unknown)
      {
         best.dxvk_client = mapped_client;
      }
      best.stack_from_module_scan = module_is_only_evidence;
      if (best.translation_version[0] == '\0')
         best.translation_version = version;
      if (best.engine_name[0] == '\0')
         copy_text(best.engine_name, "DXVK");
      if (mapped_client != DxvkClientApi::unknown)
         break;
   }
   std::fclose(maps);
   metadata = best;
}

} // namespace

ApplicationMetadata copy_application_metadata(const VkApplicationInfo *application_info) noexcept
{
   ApplicationMetadata result{};
   if (application_info != nullptr)
   {
      copy_text(result.application_name, application_info->pApplicationName);
      copy_text(result.engine_name, application_info->pEngineName);
      result.application_version = application_info->applicationVersion;
      result.engine_version = application_info->engineVersion;
      result.api_version = application_info->apiVersion;
   }

   if (text_equals(result.engine_name.data(), "DXVK"))
   {
      result.stack = ApiStackKind::dxvk;
      if ((result.application_version & 1u) != 0)
         result.dxvk_client = DxvkClientApi::d3d9;
      if (result.engine_version != 0)
         copy_vk_version(result.translation_version, result.engine_version);
   }
   else if (text_equals(result.engine_name.data(), "mesa zink"))
   {
      result.stack = ApiStackKind::zink;
      discover_mapped_mesa(result);
   }
   else if (text_equals(result.engine_name.data(), "vkd3d") ||
            text_equals(result.engine_name.data(), "VKD3D-Proton"))
   {
      result.stack = ApiStackKind::vkd3d;
      if (result.engine_version != 0)
         copy_vk_version(result.translation_version, result.engine_version);
   }

   const bool legacy_dxvk_without_client_flag =
      result.stack == ApiStackKind::dxvk && result.dxvk_client == DxvkClientApi::unknown &&
      result.engine_version != 0 && VK_VERSION_MAJOR(result.engine_version) < 2;
   if (result.stack == ApiStackKind::native_vulkan ||
       (result.stack == ApiStackKind::dxvk &&
        (result.dxvk_client == DxvkClientApi::unknown ||
         legacy_dxvk_without_client_flag || result.translation_version[0] == '\0')))
   {
      discover_mapped_dxvk(result);
   }

   char path[PATH_MAX]{};
   const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
   if (length > 0)
   {
      path[length] = '\0';
      copy_text(result.executable, basename(path));
   }
   if (result.executable[0] == '\0')
   {
      copy_text(result.executable, result.application_name[0] != '\0'
                                      ? result.application_name.data()
                                      : "unknown");
   }
   return result;
}

int application_metadata_priority(const ApplicationMetadata &metadata) noexcept
{
   if (metadata.stack != ApiStackKind::native_vulkan)
      return metadata.stack_from_module_scan ? 80 : 100;
   if (metadata.engine_name[0] != '\0')
      return 30;
   if (metadata.application_name[0] != '\0')
      return 20;
   return metadata.api_version != 0 ? 10 : 0;
}

std::string classify_api_stack(const ApplicationMetadata &metadata)
{
   const std::string_view engine(metadata.engine_name.data());
   if (metadata.stack == ApiStackKind::zink || engine == "mesa zink")
   {
      std::string result = "OpenGL -> Zink";
      if (metadata.translation_version[0] != '\0')
      {
         result += " ";
         result += metadata.translation_version.data();
      }
      result += " -> Vulkan";
      return result;
   }
   if (metadata.stack == ApiStackKind::dxvk || engine == "DXVK")
   {
      std::string result;
      DxvkClientApi client = metadata.dxvk_client;
      if (client == DxvkClientApi::unknown && (metadata.application_version & 1u) != 0)
         client = DxvkClientApi::d3d9;
      switch (client)
      {
      case DxvkClientApi::d3d8:
         result = "D3D8 -> DXVK";
         break;
      case DxvkClientApi::d3d9:
         result = "D3D9 -> DXVK";
         break;
      case DxvkClientApi::d3d10:
         result = "D3D10 -> DXVK";
         break;
      case DxvkClientApi::d3d11:
         result = "D3D11 -> DXVK";
         break;
      default:
         result = "DXVK";
         break;
      }
      if (metadata.translation_version[0] != '\0')
      {
         result += " ";
         result += metadata.translation_version.data();
      }
      result += " -> Vulkan";
      return result;
   }
   if (metadata.stack == ApiStackKind::vkd3d || engine == "vkd3d" || engine == "VKD3D-Proton")
   {
      std::string result = "D3D12 -> VKD3D-Proton";
      if (metadata.engine_version != 0)
      {
         result += " ";
         result += format_vk_version(metadata.engine_version);
      }
      result += " -> Vulkan";
      return result;
   }

   std::string result = "Vulkan";
   if (!engine.empty())
   {
      result += " (";
      result += engine;
      result += ")";
   }
   return result;
}

std::string format_vk_version(uint32_t version)
{
   char text[48]{};
   std::snprintf(text, sizeof(text), "%u.%u.%u", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                 VK_VERSION_PATCH(version));
   return text;
}

std::string format_driver_version(uint32_t vendor_id, uint32_t version)
{
   if (vendor_id == 0x13B5)
   {
      char text[48]{};
      std::snprintf(text, sizeof(text), "%u.%u.%u", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                    VK_VERSION_PATCH(version));
      return text;
   }
   return format_vk_version(version);
}

} // namespace mali_wrapper::hud
