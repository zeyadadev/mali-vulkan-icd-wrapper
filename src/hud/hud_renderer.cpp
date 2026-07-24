/*
 * SPDX-License-Identifier: MIT
 */

#include "hud/hud_renderer.hpp"

#include "config.hpp"
#include "hud/hud_layout.hpp"
#include "hud/hud_metadata.hpp"
#include "hud/hud_runtime.hpp"
#include "hud_font_data.hpp"
#include "hud_frag_spv.hpp"
#include "hud_vert_spv.hpp"
#include "wsi/wsi_private_data.hpp"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mali_wrapper::hud
{
namespace
{

constexpr uint32_t ATLAS_WIDTH = 1024;
constexpr uint32_t ATLAS_HEIGHT = 512;
constexpr float ATLAS_FONT_HEIGHT = 32.0f;
constexpr uint32_t FIRST_GLYPH = 32;
constexpr uint32_t LAST_GLYPH = 126;
constexpr uint32_t GLYPH_COUNT = LAST_GLYPH - FIRST_GLYPH + 1;
constexpr uint32_t MAX_GLYPHS = 1536;
constexpr uint32_t MAX_VERTICES = 6 + MAX_GLYPHS * 6;

struct Glyph
{
   float u0{};
   float v0{};
   float u1{};
   float v1{};
   float x_offset{};
   float y_offset{};
   float width{};
   float height{};
   float advance{};
};

struct Vertex
{
   float x;
   float y;
   float u;
   float v;
   float r;
   float g;
   float b;
   float a;
};

template <size_t N>
void copy_text(std::array<char, N> &destination, const char *source) noexcept
{
   if (source != nullptr)
   {
      std::strncpy(destination.data(), source, destination.size() - 1);
      destination.back() = '\0';
   }
}

bool required_functions_available(const device_private_data &device) noexcept
{
#define HUD_DEVICE_FUNCTION(name) device.disp.get_fn<PFN_vk##name>("vk" #name).has_value()
   return HUD_DEVICE_FUNCTION(CreateBuffer) && HUD_DEVICE_FUNCTION(DestroyBuffer) &&
          HUD_DEVICE_FUNCTION(GetBufferMemoryRequirements) && HUD_DEVICE_FUNCTION(BindBufferMemory) &&
          HUD_DEVICE_FUNCTION(FlushMappedMemoryRanges) && HUD_DEVICE_FUNCTION(CreateImageView) &&
          HUD_DEVICE_FUNCTION(DestroyImageView) && HUD_DEVICE_FUNCTION(CreateSampler) &&
          HUD_DEVICE_FUNCTION(DestroySampler) && HUD_DEVICE_FUNCTION(CreateDescriptorSetLayout) &&
          HUD_DEVICE_FUNCTION(DestroyDescriptorSetLayout) && HUD_DEVICE_FUNCTION(CreateDescriptorPool) &&
          HUD_DEVICE_FUNCTION(DestroyDescriptorPool) && HUD_DEVICE_FUNCTION(AllocateDescriptorSets) &&
          HUD_DEVICE_FUNCTION(UpdateDescriptorSets) && HUD_DEVICE_FUNCTION(CreateRenderPass) &&
          HUD_DEVICE_FUNCTION(DestroyRenderPass) && HUD_DEVICE_FUNCTION(CreateFramebuffer) &&
          HUD_DEVICE_FUNCTION(DestroyFramebuffer) && HUD_DEVICE_FUNCTION(CreateShaderModule) &&
          HUD_DEVICE_FUNCTION(DestroyShaderModule) && HUD_DEVICE_FUNCTION(CreatePipelineLayout) &&
          HUD_DEVICE_FUNCTION(DestroyPipelineLayout) && HUD_DEVICE_FUNCTION(CreatePipelineCache) &&
          HUD_DEVICE_FUNCTION(DestroyPipelineCache) && HUD_DEVICE_FUNCTION(CreateGraphicsPipelines) &&
          HUD_DEVICE_FUNCTION(DestroyPipeline) && HUD_DEVICE_FUNCTION(CmdPipelineBarrier) &&
          HUD_DEVICE_FUNCTION(CmdCopyBufferToImage) && HUD_DEVICE_FUNCTION(CmdUpdateBuffer) &&
          HUD_DEVICE_FUNCTION(CmdBeginRenderPass) &&
          HUD_DEVICE_FUNCTION(CmdEndRenderPass) && HUD_DEVICE_FUNCTION(CmdBindPipeline) &&
          HUD_DEVICE_FUNCTION(CmdBindDescriptorSets) && HUD_DEVICE_FUNCTION(CmdBindVertexBuffers) &&
          HUD_DEVICE_FUNCTION(CmdDrawIndirect) &&
          HUD_DEVICE_FUNCTION(CmdSetViewport) && HUD_DEVICE_FUNCTION(CmdSetScissor);
#undef HUD_DEVICE_FUNCTION
}

bool find_memory_type(const VkPhysicalDeviceMemoryProperties &properties, uint32_t bits,
                      VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred, uint32_t &index,
                      VkMemoryPropertyFlags &selected_flags) noexcept
{
   for (uint32_t pass = 0; pass < 2; ++pass)
   {
      const VkMemoryPropertyFlags wanted = pass == 0 ? required | preferred : required;
      for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
      {
         if ((bits & (1u << i)) != 0 &&
             (properties.memoryTypes[i].propertyFlags & wanted) == wanted)
         {
            index = i;
            selected_flags = properties.memoryTypes[i].propertyFlags;
            return true;
         }
      }
   }
   return false;
}

} // namespace

class HudDeviceResources : public std::enable_shared_from_this<HudDeviceResources>
{
public:
   static std::shared_ptr<HudDeviceResources> acquire(device_private_data &device, VkQueue queue,
                                                      const VkAllocationCallbacks *allocator) noexcept
   {
      struct RegistryEntry
      {
         VkDevice device;
         std::weak_ptr<HudDeviceResources> resources;
      };
      static std::mutex registry_mutex;
      static std::vector<RegistryEntry> registry;

      try
      {
         std::lock_guard<std::mutex> lock(registry_mutex);
         for (auto iterator = registry.begin(); iterator != registry.end();)
         {
            if (auto existing = iterator->resources.lock())
            {
               if (iterator->device == device.device)
                  return existing;
               ++iterator;
            }
            else
            {
               iterator = registry.erase(iterator);
            }
         }

         auto result = std::shared_ptr<HudDeviceResources>(new HudDeviceResources(device, allocator));
         if (!result->initialize(queue))
         {
            return {};
         }
         registry.push_back({ device.device, result });
         return result;
      }
      catch (...)
      {
         HudRuntime::instance().log_disable_once("device resource initialization threw an exception");
         return {};
      }
   }

   ~HudDeviceResources()
   {
      const VkAllocationCallbacks *callbacks = allocation_callbacks;
      if (pipeline_cache != VK_NULL_HANDLE)
         device.disp.DestroyPipelineCache(device.device, pipeline_cache, callbacks);
      if (pipeline_layout != VK_NULL_HANDLE)
         device.disp.DestroyPipelineLayout(device.device, pipeline_layout, callbacks);
      if (vertex_shader != VK_NULL_HANDLE)
         device.disp.DestroyShaderModule(device.device, vertex_shader, callbacks);
      if (fragment_shader != VK_NULL_HANDLE)
         device.disp.DestroyShaderModule(device.device, fragment_shader, callbacks);
      if (descriptor_pool != VK_NULL_HANDLE)
         device.disp.DestroyDescriptorPool(device.device, descriptor_pool, callbacks);
      if (descriptor_layout != VK_NULL_HANDLE)
         device.disp.DestroyDescriptorSetLayout(device.device, descriptor_layout, callbacks);
      if (sampler != VK_NULL_HANDLE)
         device.disp.DestroySampler(device.device, sampler, callbacks);
      if (atlas_view != VK_NULL_HANDLE)
         device.disp.DestroyImageView(device.device, atlas_view, callbacks);
      if (atlas != VK_NULL_HANDLE)
         device.disp.DestroyImage(device.device, atlas, callbacks);
      if (atlas_memory != VK_NULL_HANDLE)
         device.disp.FreeMemory(device.device, atlas_memory, callbacks);
   }

   device_private_data &device;
   const VkAllocationCallbacks *allocation_callbacks{};
   VkPhysicalDeviceMemoryProperties memory_properties{};
   DriverMetadata driver_metadata{};
   std::array<Glyph, GLYPH_COUNT> glyphs{};
   float ascent{};
   float descent{};
   float line_gap{};
   VkImage atlas{ VK_NULL_HANDLE };
   VkDeviceMemory atlas_memory{ VK_NULL_HANDLE };
   VkImageView atlas_view{ VK_NULL_HANDLE };
   VkSampler sampler{ VK_NULL_HANDLE };
   VkDescriptorSetLayout descriptor_layout{ VK_NULL_HANDLE };
   VkDescriptorPool descriptor_pool{ VK_NULL_HANDLE };
   VkDescriptorSet descriptor_set{ VK_NULL_HANDLE };
   VkPipelineLayout pipeline_layout{ VK_NULL_HANDLE };
   VkPipelineCache pipeline_cache{ VK_NULL_HANDLE };
   VkShaderModule vertex_shader{ VK_NULL_HANDLE };
   VkShaderModule fragment_shader{ VK_NULL_HANDLE };

private:
   explicit HudDeviceResources(device_private_data &device_data, const VkAllocationCallbacks *allocator)
      : device(device_data)
      , allocation_callbacks(allocator)
   {
   }

   bool initialize(VkQueue queue)
   {
      if (!required_functions_available(device) ||
          !device.instance_data.disp.get_fn<PFN_vkGetPhysicalDeviceMemoryProperties>(
             "vkGetPhysicalDeviceMemoryProperties"))
      {
         HudRuntime::instance().log_disable_once("required Vulkan 1.0 entrypoints are unavailable");
         return false;
      }
      device.instance_data.disp.GetPhysicalDeviceMemoryProperties(device.physical_device, &memory_properties);
      collect_driver_metadata();

      std::vector<uint8_t> pixels;
      if (!build_atlas(pixels) || HudRuntime::instance().config().test_fault == HudFault::atlas)
      {
         HudRuntime::instance().log_disable_once("font atlas generation failed");
         return false;
      }
      if (!upload_atlas(queue, pixels))
      {
         HudRuntime::instance().log_disable_once("font atlas upload failed");
         return false;
      }
      return create_descriptors_and_shaders();
   }

   void collect_driver_metadata() noexcept
   {
      VkPhysicalDeviceProperties legacy{};
      device.instance_data.disp.GetPhysicalDeviceProperties(device.physical_device, &legacy);
      copy_text(driver_metadata.device_name, legacy.deviceName);
      driver_metadata.api_version = legacy.apiVersion;
      driver_metadata.driver_version = legacy.driverVersion;

      auto properties2 =
         device.instance_data.disp.get_fn<PFN_vkGetPhysicalDeviceProperties2KHR>("vkGetPhysicalDeviceProperties2KHR");
      if (properties2)
      {
         VkPhysicalDeviceDriverProperties driver{};
         driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
         VkPhysicalDeviceProperties2 properties{};
         properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
         properties.pNext = &driver;
         (*properties2)(device.physical_device, &properties);
         copy_text(driver_metadata.device_name, properties.properties.deviceName);
         driver_metadata.api_version = properties.properties.apiVersion;
         driver_metadata.driver_version = properties.properties.driverVersion;
         driver_metadata.driver_id = static_cast<uint32_t>(driver.driverID);
         copy_text(driver_metadata.driver_name, driver.driverName);
         copy_text(driver_metadata.driver_info, driver.driverInfo);
      }
      if (driver_metadata.driver_name[0] == '\0')
      {
         copy_text(driver_metadata.driver_name, "Mali");
      }
   }

   bool build_atlas(std::vector<uint8_t> &pixels)
   {
      stbtt_fontinfo font{};
      const int offset = stbtt_GetFontOffsetForIndex(mali_hud_font_data, 0);
      if (offset < 0 || !stbtt_InitFont(&font, mali_hud_font_data, offset))
         return false;

      pixels.assign(static_cast<size_t>(ATLAS_WIDTH) * ATLAS_HEIGHT, 0);
      pixels[0] = 255;
      const float scale = stbtt_ScaleForPixelHeight(&font, ATLAS_FONT_HEIGHT);
      int ascent_units = 0;
      int descent_units = 0;
      int gap_units = 0;
      stbtt_GetFontVMetrics(&font, &ascent_units, &descent_units, &gap_units);
      ascent = static_cast<float>(ascent_units) * scale;
      descent = static_cast<float>(descent_units) * scale;
      line_gap = static_cast<float>(gap_units) * scale;

      int cursor_x = 2;
      int cursor_y = 2;
      int row_height = 0;
      for (uint32_t codepoint = FIRST_GLYPH; codepoint <= LAST_GLYPH; ++codepoint)
      {
         int width = 0;
         int height = 0;
         int x_offset = 0;
         int y_offset = 0;
         unsigned char *sdf = stbtt_GetCodepointSDF(&font, scale, static_cast<int>(codepoint), 5, 128, 32.0f,
                                                    &width, &height, &x_offset, &y_offset);
         int advance_units = 0;
         int bearing = 0;
         stbtt_GetCodepointHMetrics(&font, static_cast<int>(codepoint), &advance_units, &bearing);
         Glyph &glyph = glyphs[codepoint - FIRST_GLYPH];
         glyph.advance = static_cast<float>(advance_units) * scale;
         if (sdf == nullptr || width == 0 || height == 0)
         {
            glyph.u0 = glyph.v0 = glyph.u1 = glyph.v1 = 0.0f;
            continue;
         }
         if (cursor_x + width + 1 > static_cast<int>(ATLAS_WIDTH))
         {
            cursor_x = 2;
            cursor_y += row_height + 1;
            row_height = 0;
         }
         if (cursor_y + height + 1 > static_cast<int>(ATLAS_HEIGHT))
         {
            stbtt_FreeSDF(sdf, nullptr);
            return false;
         }

         for (int row = 0; row < height; ++row)
         {
            std::memcpy(pixels.data() + static_cast<size_t>(cursor_y + row) * ATLAS_WIDTH + cursor_x,
                        sdf + static_cast<size_t>(row) * width, static_cast<size_t>(width));
         }
         glyph.u0 = static_cast<float>(cursor_x) / ATLAS_WIDTH;
         glyph.v0 = static_cast<float>(cursor_y) / ATLAS_HEIGHT;
         glyph.u1 = static_cast<float>(cursor_x + width) / ATLAS_WIDTH;
         glyph.v1 = static_cast<float>(cursor_y + height) / ATLAS_HEIGHT;
         glyph.x_offset = static_cast<float>(x_offset);
         glyph.y_offset = static_cast<float>(y_offset);
         glyph.width = static_cast<float>(width);
         glyph.height = static_cast<float>(height);
         cursor_x += width + 1;
         row_height = std::max(row_height, height);
         stbtt_FreeSDF(sdf, nullptr);
      }
      return true;
   }

   bool upload_atlas(VkQueue queue, const std::vector<uint8_t> &pixels) noexcept
   {
      const VkAllocationCallbacks *callbacks = allocation_callbacks;
      VkImageCreateInfo image_info{};
      image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      image_info.imageType = VK_IMAGE_TYPE_2D;
      image_info.format = VK_FORMAT_R8_UNORM;
      image_info.extent = { ATLAS_WIDTH, ATLAS_HEIGHT, 1 };
      image_info.mipLevels = 1;
      image_info.arrayLayers = 1;
      image_info.samples = VK_SAMPLE_COUNT_1_BIT;
      image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
      image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (device.disp.CreateImage(device.device, &image_info, callbacks, &atlas) != VK_SUCCESS)
         return false;

      VkMemoryRequirements image_requirements{};
      device.disp.GetImageMemoryRequirements(device.device, atlas, &image_requirements);
      uint32_t image_type = 0;
      VkMemoryPropertyFlags image_flags = 0;
      if (!find_memory_type(memory_properties, image_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                            0, image_type, image_flags))
         return false;
      VkMemoryAllocateInfo image_allocation{};
      image_allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      image_allocation.allocationSize = image_requirements.size;
      image_allocation.memoryTypeIndex = image_type;
      if (device.disp.AllocateMemory(device.device, &image_allocation, callbacks, &atlas_memory) != VK_SUCCESS ||
          device.disp.BindImageMemory(device.device, atlas, atlas_memory, 0) != VK_SUCCESS)
         return false;

      VkBuffer staging = VK_NULL_HANDLE;
      VkDeviceMemory staging_memory = VK_NULL_HANDLE;
      VkCommandPool command_pool = VK_NULL_HANDLE;
      VkFence fence = VK_NULL_HANDLE;
      auto cleanup = [&] {
         if (fence != VK_NULL_HANDLE)
            device.disp.DestroyFence(device.device, fence, callbacks);
         if (command_pool != VK_NULL_HANDLE)
            device.disp.DestroyCommandPool(device.device, command_pool, callbacks);
         if (staging != VK_NULL_HANDLE)
            device.disp.DestroyBuffer(device.device, staging, callbacks);
         if (staging_memory != VK_NULL_HANDLE)
            device.disp.FreeMemory(device.device, staging_memory, callbacks);
      };

      VkBufferCreateInfo buffer_info{};
      buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      buffer_info.size = pixels.size();
      buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      if (device.disp.CreateBuffer(device.device, &buffer_info, callbacks, &staging) != VK_SUCCESS)
      {
         cleanup();
         return false;
      }
      VkMemoryRequirements buffer_requirements{};
      device.disp.GetBufferMemoryRequirements(device.device, staging, &buffer_requirements);
      uint32_t buffer_type = 0;
      VkMemoryPropertyFlags buffer_flags = 0;
      if (!find_memory_type(memory_properties, buffer_requirements.memoryTypeBits,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, buffer_type, buffer_flags))
      {
         cleanup();
         return false;
      }
      VkMemoryAllocateInfo buffer_allocation{};
      buffer_allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      buffer_allocation.allocationSize = buffer_requirements.size;
      buffer_allocation.memoryTypeIndex = buffer_type;
      if (device.disp.AllocateMemory(device.device, &buffer_allocation, callbacks, &staging_memory) != VK_SUCCESS ||
          device.disp.BindBufferMemory(device.device, staging, staging_memory, 0) != VK_SUCCESS)
      {
         cleanup();
         return false;
      }
      VkCommandPoolCreateInfo pool_info{};
      pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      pool_info.queueFamilyIndex = 0;
      if (device.disp.CreateCommandPool(device.device, &pool_info, callbacks, &command_pool) != VK_SUCCESS)
      {
         cleanup();
         return false;
      }
      VkCommandBuffer command = VK_NULL_HANDLE;
      VkCommandBufferAllocateInfo command_info{};
      command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      command_info.commandPool = command_pool;
      command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      command_info.commandBufferCount = 1;
      if (device.disp.AllocateCommandBuffers(device.device, &command_info, &command) != VK_SUCCESS)
      {
         cleanup();
         return false;
      }
      VkCommandBufferBeginInfo begin{};
      begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      if (device.disp.BeginCommandBuffer(command, &begin) != VK_SUCCESS)
      {
         cleanup();
         return false;
      }

      constexpr VkDeviceSize MAX_UPDATE = 65536;
      VkDeviceSize update_offset = 0;
      while (update_offset < pixels.size())
      {
         const VkDeviceSize update_size =
            std::min<VkDeviceSize>(MAX_UPDATE, pixels.size() - update_offset);
         device.disp.CmdUpdateBuffer(command, staging, update_offset, update_size,
                                     pixels.data() + update_offset);
         update_offset += update_size;
      }
      VkMemoryBarrier staging_ready{};
      staging_ready.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      staging_ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      staging_ready.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      device.disp.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &staging_ready, 0,
                                     nullptr, 0, nullptr);

      VkImageMemoryBarrier to_transfer{};
      to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      to_transfer.srcAccessMask = 0;
      to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_transfer.image = atlas;
      to_transfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      device.disp.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                     0, nullptr, 0, nullptr, 1, &to_transfer);
      VkBufferImageCopy copy{};
      copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
      copy.imageExtent = { ATLAS_WIDTH, ATLAS_HEIGHT, 1 };
      device.disp.CmdCopyBufferToImage(command, staging, atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

      VkImageMemoryBarrier to_shader = to_transfer;
      to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      device.disp.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &to_shader);
      if (device.disp.EndCommandBuffer(command) != VK_SUCCESS)
      {
         cleanup();
         return false;
      }
      VkFenceCreateInfo fence_info{};
      fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      if (device.disp.CreateFence(device.device, &fence_info, callbacks, &fence) != VK_SUCCESS)
      {
         cleanup();
         return false;
      }
      VkSubmitInfo submit{};
      submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &command;
      const VkResult submitted = device.disp.QueueSubmit(queue, 1, &submit, fence);
      const VkResult waited =
         submitted == VK_SUCCESS ? device.disp.WaitForFences(device.device, 1, &fence, VK_TRUE, UINT64_MAX) : submitted;
      cleanup();
      return submitted == VK_SUCCESS && waited == VK_SUCCESS;
   }

   bool create_descriptors_and_shaders() noexcept
   {
      const VkAllocationCallbacks *callbacks = allocation_callbacks;
      VkImageViewCreateInfo view_info{};
      view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      view_info.image = atlas;
      view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view_info.format = VK_FORMAT_R8_UNORM;
      view_info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      if (device.disp.CreateImageView(device.device, &view_info, callbacks, &atlas_view) != VK_SUCCESS)
         return false;

      VkSamplerCreateInfo sampler_info{};
      sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
      sampler_info.magFilter = VK_FILTER_LINEAR;
      sampler_info.minFilter = VK_FILTER_LINEAR;
      sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      sampler_info.maxLod = 0.0f;
      if (device.disp.CreateSampler(device.device, &sampler_info, callbacks, &sampler) != VK_SUCCESS)
         return false;

      VkDescriptorSetLayoutBinding binding{};
      binding.binding = 0;
      binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      binding.descriptorCount = 1;
      binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
      VkDescriptorSetLayoutCreateInfo layout_info{};
      layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      layout_info.bindingCount = 1;
      layout_info.pBindings = &binding;
      if (device.disp.CreateDescriptorSetLayout(device.device, &layout_info, callbacks, &descriptor_layout) !=
          VK_SUCCESS)
         return false;

      VkDescriptorPoolSize pool_size{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
      VkDescriptorPoolCreateInfo pool_info{};
      pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
      pool_info.maxSets = 1;
      pool_info.poolSizeCount = 1;
      pool_info.pPoolSizes = &pool_size;
      if (device.disp.CreateDescriptorPool(device.device, &pool_info, callbacks,
                                           &descriptor_pool) != VK_SUCCESS)
         return false;
      VkDescriptorSetAllocateInfo descriptor_allocation{};
      descriptor_allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      descriptor_allocation.descriptorPool = descriptor_pool;
      descriptor_allocation.descriptorSetCount = 1;
      descriptor_allocation.pSetLayouts = &descriptor_layout;
      if (device.disp.AllocateDescriptorSets(device.device, &descriptor_allocation,
                                              &descriptor_set) != VK_SUCCESS)
         return false;
      VkDescriptorImageInfo descriptor_image{ sampler, atlas_view,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
      VkWriteDescriptorSet descriptor_write{};
      descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      descriptor_write.dstSet = descriptor_set;
      descriptor_write.dstBinding = 0;
      descriptor_write.descriptorCount = 1;
      descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      descriptor_write.pImageInfo = &descriptor_image;
      device.disp.UpdateDescriptorSets(device.device, 1, &descriptor_write, 0, nullptr);

      VkPipelineLayoutCreateInfo pipeline_layout_info{};
      pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
      pipeline_layout_info.setLayoutCount = 1;
      pipeline_layout_info.pSetLayouts = &descriptor_layout;
      if (device.disp.CreatePipelineLayout(device.device, &pipeline_layout_info, callbacks, &pipeline_layout) !=
          VK_SUCCESS)
         return false;

      VkPipelineCacheCreateInfo cache_info{};
      cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
      if (device.disp.CreatePipelineCache(device.device, &cache_info, callbacks, &pipeline_cache) != VK_SUCCESS)
         return false;

      VkShaderModuleCreateInfo shader_info{};
      shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      shader_info.codeSize = sizeof(mali_hud_vert_spv);
      shader_info.pCode = mali_hud_vert_spv;
      if (device.disp.CreateShaderModule(device.device, &shader_info, callbacks, &vertex_shader) != VK_SUCCESS)
         return false;
      shader_info.codeSize = sizeof(mali_hud_frag_spv);
      shader_info.pCode = mali_hud_frag_spv;
      return device.disp.CreateShaderModule(device.device, &shader_info, callbacks, &fragment_shader) == VK_SUCCESS;
   }
};

struct HudSwapchainResources::Impl
{
   struct ImageResources
   {
      VkImageView view{ VK_NULL_HANDLE };
      VkFramebuffer framebuffer{ VK_NULL_HANDLE };
      VkBuffer buffer{ VK_NULL_HANDLE };
      VkDeviceMemory memory{ VK_NULL_HANDLE };
      void *mapped{ nullptr };
      bool coherent{ false };
      VkCommandBuffer command{ VK_NULL_HANDLE };
   };

   Impl(device_private_data &device_data, VkQueue present_queue,
        const VkSwapchainCreateInfoKHR &info, const char *server_name,
        const VkAllocationCallbacks *callbacks)
      : device(device_data)
      , queue(present_queue)
      , create_info(info)
      , allocation_callbacks(callbacks)
   {
      std::snprintf(display_server.data(), display_server.size(), "%s",
                    server_name != nullptr ? server_name : "Unknown");
   }

   ~Impl()
   {
      if (runtime_id != 0)
         HudRuntime::instance().unregister_swapchain(runtime_id);
      for (ImageResources &image : images)
      {
         if (image.mapped != nullptr)
            device.disp.UnmapMemory(device.device, image.memory);
         if (image.buffer != VK_NULL_HANDLE)
            device.disp.DestroyBuffer(device.device, image.buffer, allocation_callbacks);
         if (image.memory != VK_NULL_HANDLE)
            device.disp.FreeMemory(device.device, image.memory, allocation_callbacks);
         if (image.framebuffer != VK_NULL_HANDLE)
            device.disp.DestroyFramebuffer(device.device, image.framebuffer, allocation_callbacks);
         if (image.view != VK_NULL_HANDLE)
            device.disp.DestroyImageView(device.device, image.view, allocation_callbacks);
      }
      if (command_pool != VK_NULL_HANDLE)
         device.disp.DestroyCommandPool(device.device, command_pool, allocation_callbacks);
      if (pipeline != VK_NULL_HANDLE)
         device.disp.DestroyPipeline(device.device, pipeline, allocation_callbacks);
      if (render_pass != VK_NULL_HANDLE)
         device.disp.DestroyRenderPass(device.device, render_pass, allocation_callbacks);
   }

   bool initialize(const VkImage *swapchain_images, uint32_t image_count)
   {
      shared = HudDeviceResources::acquire(device, queue, allocation_callbacks);
      if (!shared)
         return false;
      if (HudRuntime::instance().config().test_fault == HudFault::pipeline)
      {
         HudRuntime::instance().log_disable_once("pipeline fault injected");
         return false;
      }
      if (!create_render_pass() || !create_pipeline())
         return false;
      try
      {
         images.resize(image_count);
      }
      catch (...)
      {
         return false;
      }
      if (!create_images_and_buffers(swapchain_images, image_count))
         return false;

      application = HudRuntime::instance().application();
      for (char &character : application.executable)
      {
         if (character >= 'a' && character <= 'z')
            character = static_cast<char>(character - 'a' + 'A');
      }
      api_stack = classify_api_stack(application);
      const uint32_t api_version =
         application.api_version != 0 ? application.api_version : shared->driver_metadata.api_version;
      const size_t vulkan_label = api_stack.rfind("Vulkan");
      if (api_version != 0 && vulkan_label != std::string::npos)
      {
         api_stack.insert(vulkan_label + std::strlen("Vulkan"),
                          " " + format_vk_version(api_version));
      }
      driver_version = format_driver_version(0x13B5, shared->driver_metadata.driver_version);
      if (shared->driver_metadata.driver_info[0] != '\0')
      {
         driver_version = shared->driver_metadata.driver_info.data();
         const size_t first_separator = driver_version.find('.');
         const size_t build_separator =
            first_separator == std::string::npos ? std::string::npos
                                                 : driver_version.find('.', first_separator + 1);
         if (build_separator != std::string::npos)
            driver_version.resize(build_separator);
      }
      compute_panel();
      for (ImageResources &image : images)
      {
         if (!record_command(image))
            return false;
      }
      runtime_id = HudRuntime::instance().register_swapchain(create_info.imageExtent.width,
                                                              create_info.imageExtent.height, true);
      if (runtime_id == 0)
         return false;
      if (HudRuntime::instance().config().debug)
      {
         std::fprintf(stderr, "mali-hud: renderer active (%ux%u, %zu images)\n",
                      create_info.imageExtent.width, create_info.imageExtent.height, images.size());
         std::fprintf(stderr, "mali-hud: framebuffer format=%u\n",
                      static_cast<unsigned>(create_info.imageFormat));
      }
      return true;
   }

   bool create_render_pass() noexcept
   {
      VkAttachmentDescription attachment{};
      attachment.format = create_info.imageFormat;
      attachment.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      VkAttachmentReference color{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
      VkSubpassDescription subpass{};
      subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      subpass.colorAttachmentCount = 1;
      subpass.pColorAttachments = &color;
      VkSubpassDependency dependencies[2]{};
      dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
      dependencies[0].dstSubpass = 0;
      dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      dependencies[1].srcSubpass = 0;
      dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
      dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
      dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      VkRenderPassCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
      info.attachmentCount = 1;
      info.pAttachments = &attachment;
      info.subpassCount = 1;
      info.pSubpasses = &subpass;
      info.dependencyCount = 2;
      info.pDependencies = dependencies;
      return device.disp.CreateRenderPass(device.device, &info, allocation_callbacks, &render_pass) == VK_SUCCESS;
   }

   bool create_pipeline() noexcept
   {
      VkPipelineShaderStageCreateInfo stages[2]{};
      stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
      stages[0].module = shared->vertex_shader;
      stages[0].pName = "main";
      stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      stages[1].module = shared->fragment_shader;
      stages[1].pName = "main";
      VkVertexInputBindingDescription binding{ 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
      VkVertexInputAttributeDescription attributes[3] = {
         { 0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, x) },
         { 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u) },
         { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, r) },
      };
      VkPipelineVertexInputStateCreateInfo vertex_input{};
      vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
      vertex_input.vertexBindingDescriptionCount = 1;
      vertex_input.pVertexBindingDescriptions = &binding;
      vertex_input.vertexAttributeDescriptionCount = 3;
      vertex_input.pVertexAttributeDescriptions = attributes;
      VkPipelineInputAssemblyStateCreateInfo assembly{};
      assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
      assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(create_info.imageExtent.width),
                           static_cast<float>(create_info.imageExtent.height), 0.0f, 1.0f };
      VkRect2D scissor{ { 0, 0 }, create_info.imageExtent };
      VkPipelineViewportStateCreateInfo viewport_state{};
      viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
      viewport_state.viewportCount = 1;
      viewport_state.pViewports = &viewport;
      viewport_state.scissorCount = 1;
      viewport_state.pScissors = &scissor;
      VkPipelineRasterizationStateCreateInfo rasterization{};
      rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
      rasterization.polygonMode = VK_POLYGON_MODE_FILL;
      rasterization.cullMode = VK_CULL_MODE_NONE;
      rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
      rasterization.lineWidth = 1.0f;
      VkPipelineMultisampleStateCreateInfo multisample{};
      multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
      multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineDepthStencilStateCreateInfo depth_stencil{};
      depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
      VkPipelineColorBlendAttachmentState blend{};
      blend.blendEnable = VK_TRUE;
      blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      blend.colorBlendOp = VK_BLEND_OP_ADD;
      blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      blend.alphaBlendOp = VK_BLEND_OP_ADD;
      blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                             VK_COLOR_COMPONENT_A_BIT;
      VkPipelineColorBlendStateCreateInfo blending{};
      blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
      blending.attachmentCount = 1;
      blending.pAttachments = &blend;
      const VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
      VkPipelineDynamicStateCreateInfo dynamic{};
      dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
      dynamic.dynamicStateCount = 2;
      dynamic.pDynamicStates = dynamic_states;
      VkGraphicsPipelineCreateInfo pipeline_info{};
      pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
      pipeline_info.stageCount = 2;
      pipeline_info.pStages = stages;
      pipeline_info.pVertexInputState = &vertex_input;
      pipeline_info.pInputAssemblyState = &assembly;
      pipeline_info.pViewportState = &viewport_state;
      pipeline_info.pRasterizationState = &rasterization;
      pipeline_info.pMultisampleState = &multisample;
      pipeline_info.pDepthStencilState = &depth_stencil;
      pipeline_info.pColorBlendState = &blending;
      pipeline_info.pDynamicState = &dynamic;
      pipeline_info.layout = shared->pipeline_layout;
      pipeline_info.renderPass = render_pass;
      pipeline_info.subpass = 0;
      pipeline_info.basePipelineIndex = -1;
      const VkResult result = device.disp.CreateGraphicsPipelines(
         device.device, shared->pipeline_cache, 1, &pipeline_info, allocation_callbacks, &pipeline);
      if (HudRuntime::instance().config().debug)
      {
         std::fprintf(stderr, "mali-hud: graphics pipeline result=%d handle=%p\n", result,
                      reinterpret_cast<void *>(pipeline));
      }
      return result == VK_SUCCESS && pipeline != VK_NULL_HANDLE;
   }

   bool create_images_and_buffers(const VkImage *swapchain_images, uint32_t image_count)
   {
      VkCommandPoolCreateInfo pool{};
      pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      pool.queueFamilyIndex = 0;
      if (device.disp.CreateCommandPool(device.device, &pool, allocation_callbacks, &command_pool) != VK_SUCCESS)
         return false;
      std::vector<VkCommandBuffer> commands(image_count);
      VkCommandBufferAllocateInfo command_info{};
      command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      command_info.commandPool = command_pool;
      command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      command_info.commandBufferCount = image_count;
      if (device.disp.AllocateCommandBuffers(device.device, &command_info, commands.data()) != VK_SUCCESS)
         return false;

      constexpr VkDeviceSize vertex_bytes = sizeof(Vertex) * MAX_VERTICES;
      indirect_offset = (vertex_bytes + 15) & ~VkDeviceSize(15);
      const VkDeviceSize buffer_size = indirect_offset + sizeof(VkDrawIndirectCommand);
      for (uint32_t i = 0; i < image_count; ++i)
      {
         ImageResources &image = images[i];
         image.command = commands[i];
         VkImageViewCreateInfo view{};
         view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
         view.image = swapchain_images[i];
         view.viewType = VK_IMAGE_VIEW_TYPE_2D;
         view.format = create_info.imageFormat;
         view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
         if (device.disp.CreateImageView(device.device, &view, allocation_callbacks, &image.view) != VK_SUCCESS)
            return false;
         VkFramebufferCreateInfo framebuffer{};
         framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
         framebuffer.renderPass = render_pass;
         framebuffer.attachmentCount = 1;
         framebuffer.pAttachments = &image.view;
         framebuffer.width = create_info.imageExtent.width;
         framebuffer.height = create_info.imageExtent.height;
         framebuffer.layers = 1;
         if (device.disp.CreateFramebuffer(device.device, &framebuffer, allocation_callbacks, &image.framebuffer) !=
             VK_SUCCESS)
            return false;

         if (HudRuntime::instance().config().test_fault == HudFault::buffer)
         {
            HudRuntime::instance().log_disable_once("buffer fault injected");
            return false;
         }
         VkBufferCreateInfo buffer{};
         buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
         buffer.size = buffer_size;
         buffer.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
         buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
         if (device.disp.CreateBuffer(device.device, &buffer, allocation_callbacks,
                                      &image.buffer) != VK_SUCCESS)
            return false;
         VkMemoryRequirements requirements{};
         device.disp.GetBufferMemoryRequirements(device.device, image.buffer, &requirements);
         uint32_t memory_type = 0;
         VkMemoryPropertyFlags flags = 0;
         if (!find_memory_type(shared->memory_properties, requirements.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memory_type, flags))
            return false;
         VkMemoryAllocateInfo allocation{};
         allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
         allocation.allocationSize = requirements.size;
         allocation.memoryTypeIndex = memory_type;
         if (device.disp.AllocateMemory(device.device, &allocation, allocation_callbacks,
                                        &image.memory) != VK_SUCCESS ||
             device.disp.BindBufferMemory(device.device, image.buffer, image.memory, 0) !=
                VK_SUCCESS ||
             device.disp.MapMemory(device.device, image.memory, 0, VK_WHOLE_SIZE, 0,
                                   &image.mapped) != VK_SUCCESS)
            return false;
         image.coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
      }
      return true;
   }

   void compute_panel() noexcept
   {
      const HudConfig &config = HudRuntime::instance().config();
      const HudPanelLayout layout =
         calculate_panel_layout(create_info.imageExtent.width,
                                create_info.imageExtent.height, config);
      scale = layout.scale;
      text_opacity = config.text_opacity;
      panel.offset = { layout.x, layout.y };
      panel.extent = { layout.width, layout.height };
   }

   bool record_command(ImageResources &image) noexcept
   {
      VkCommandBufferBeginInfo begin{};
      begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
      if (device.disp.BeginCommandBuffer(image.command, &begin) != VK_SUCCESS)
         return false;

      VkMemoryBarrier host_write{};
      host_write.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      host_write.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
      host_write.dstAccessMask =
         VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
      device.disp.CmdPipelineBarrier(image.command, VK_PIPELINE_STAGE_HOST_BIT,
                                     VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                                     0, 1, &host_write, 0, nullptr, 0, nullptr);
      VkRenderPassBeginInfo render{};
      render.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
      render.renderPass = render_pass;
      render.framebuffer = image.framebuffer;
      render.renderArea = panel;
      device.disp.CmdBeginRenderPass(image.command, &render, VK_SUBPASS_CONTENTS_INLINE);
      device.disp.CmdBindPipeline(image.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      device.disp.CmdBindDescriptorSets(image.command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        shared->pipeline_layout, 0, 1, &shared->descriptor_set, 0,
                                        nullptr);
      VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(create_info.imageExtent.width),
                           static_cast<float>(create_info.imageExtent.height), 0.0f, 1.0f };
      VkRect2D scissor{ { 0, 0 }, create_info.imageExtent };
      device.disp.CmdSetViewport(image.command, 0, 1, &viewport);
      device.disp.CmdSetScissor(image.command, 0, 1, &scissor);
      const VkDeviceSize vertex_offset = 0;
      device.disp.CmdBindVertexBuffers(image.command, 0, 1, &image.buffer,
                                       &vertex_offset);
      device.disp.CmdDrawIndirect(image.command, image.buffer, indirect_offset, 1u,
                                  static_cast<uint32_t>(sizeof(VkDrawIndirectCommand)));
      device.disp.CmdEndRenderPass(image.command);
      return device.disp.EndCommandBuffer(image.command) == VK_SUCCESS;
   }

   void add_quad(Vertex *vertices, uint32_t &count, float x0, float y0, float x1, float y1, float u0, float v0,
                 float u1, float v1, float r, float g, float b, float a) noexcept
   {
      if (count + 6 > MAX_VERTICES)
         return;
      const float width = static_cast<float>(create_info.imageExtent.width);
      const float height = static_cast<float>(create_info.imageExtent.height);
      const auto ndc_x = [width](float x) { return x * 2.0f / width - 1.0f; };
      const auto ndc_y = [height](float y) { return y * 2.0f / height - 1.0f; };
      const Vertex quad[6] = {
         { ndc_x(x0), ndc_y(y0), u0, v0, r, g, b, a },
         { ndc_x(x0), ndc_y(y1), u0, v1, r, g, b, a },
         { ndc_x(x1), ndc_y(y1), u1, v1, r, g, b, a },
         { ndc_x(x0), ndc_y(y0), u0, v0, r, g, b, a },
         { ndc_x(x1), ndc_y(y1), u1, v1, r, g, b, a },
         { ndc_x(x1), ndc_y(y0), u1, v0, r, g, b, a },
      };
      std::memcpy(vertices + count, quad, sizeof(quad));
      count += 6;
   }

   float measure_text(const char *text, float font_height) const noexcept
   {
      const float glyph_scale = (font_height * scale) / ATLAS_FONT_HEIGHT;
      float pen = 0.0f;
      float right_edge = 0.0f;
      for (const unsigned char *cursor =
              reinterpret_cast<const unsigned char *>(text);
           *cursor != 0; ++cursor)
      {
         uint32_t codepoint = *cursor;
         if (codepoint < FIRST_GLYPH || codepoint > LAST_GLYPH)
            codepoint = '?';
         const Glyph &glyph = shared->glyphs[codepoint - FIRST_GLYPH];
         right_edge =
            std::max(right_edge,
                     pen + (glyph.x_offset + glyph.width) * glyph_scale);
         pen += glyph.advance * glyph_scale;
      }
      return std::max(pen, right_edge);
   }

   void add_text(Vertex *vertices, uint32_t &count, const char *text, float x,
                 float baseline, float right_edge, float font_height) noexcept
   {
      const float glyph_scale = (font_height * scale) / ATLAS_FONT_HEIGHT;
      for (const unsigned char *cursor = reinterpret_cast<const unsigned char *>(text); *cursor != 0; ++cursor)
      {
         uint32_t codepoint = *cursor;
         if (codepoint < FIRST_GLYPH || codepoint > LAST_GLYPH)
            codepoint = '?';
         const Glyph &glyph = shared->glyphs[codepoint - FIRST_GLYPH];
         const float x0 = x + glyph.x_offset * glyph_scale;
         const float y0 = baseline + glyph.y_offset * glyph_scale;
         const float x1 = x0 + glyph.width * glyph_scale;
         const float y1 = y0 + glyph.height * glyph_scale;
         if (x1 > right_edge)
            break;
         if (glyph.width != 0.0f && glyph.height != 0.0f)
         {
            add_quad(vertices, count, x0, y0, x1, y1, glyph.u0, glyph.v0, glyph.u1, glyph.v1, 0.93f, 0.96f,
                     1.0f, text_opacity);
         }
         x += glyph.advance * glyph_scale;
      }
   }

   static void metric(char *destination, size_t size, float value, const char *format) noexcept
   {
      if (std::isnan(value))
         std::snprintf(destination, size, "N/A");
      else
         std::snprintf(destination, size, format, value);
   }

   bool rebuild(ImageResources &image, const MetricsSnapshot &snapshot) noexcept
   {
      auto *vertices = static_cast<Vertex *>(image.mapped);
      uint32_t count = 0;
      const HudConfig &config = HudRuntime::instance().config();

      char fps[24]{};
      char frame_time[24]{};
      char system_cpu[24]{};
      char process_cpu[24]{};
      char cpu_temp[24]{};
      char ram_used[24]{};
      char ram_total[24]{};
      char process_ram[24]{};
      char gpu[24]{};
      char gpu_clock[24]{};
      char gpu_temp[24]{};
      metric(fps, sizeof(fps), snapshot.fps, "%.1f");
      metric(frame_time, sizeof(frame_time), snapshot.frame_time_ms, "%.2f");
      metric(system_cpu, sizeof(system_cpu), snapshot.system_cpu_percent, "%.0f%%");
      metric(process_cpu, sizeof(process_cpu), snapshot.process_cpu_percent, "%.0f%%");
      metric(cpu_temp, sizeof(cpu_temp), snapshot.cpu_temp_c, "%.0f C");
      metric(ram_used, sizeof(ram_used), snapshot.system_ram_used_gib, "%.1f");
      metric(ram_total, sizeof(ram_total), snapshot.system_ram_total_gib, "%.1f GiB");
      metric(process_ram, sizeof(process_ram), snapshot.process_ram_mib, "%.0f MiB");
      metric(gpu, sizeof(gpu), snapshot.gpu_percent, "%.0f%%");
      metric(gpu_clock, sizeof(gpu_clock), snapshot.gpu_clock_mhz, "%.0f MHz");
      metric(gpu_temp, sizeof(gpu_temp), snapshot.gpu_temp_c, "%.0f C");

      char lines[HUD_LINE_COUNT][256]{};
      std::snprintf(lines[0], sizeof(lines[0]), "EXE  %s", application.executable.data());
      std::snprintf(lines[1], sizeof(lines[1]), "API  %s", api_stack.c_str());
      std::snprintf(lines[2], sizeof(lines[2]), "DISPLAY %ux%u  |  %s",
                    create_info.imageExtent.width, create_info.imageExtent.height,
                    display_server.data());
      std::snprintf(lines[3], sizeof(lines[3]), "FPS  %s  |  %s ms", fps, frame_time);
      std::snprintf(lines[4], sizeof(lines[4]), "CPU  %s system  |  %s process  |  %s", system_cpu, process_cpu,
                    cpu_temp);
      std::snprintf(lines[5], sizeof(lines[5]), "RAM  %s / %s system  |  %s process",
                    ram_used, ram_total, process_ram);
      std::snprintf(lines[6], sizeof(lines[6]), "GPU  %s  |  %s  |  %s", gpu, gpu_clock, gpu_temp);
      std::snprintf(lines[7], sizeof(lines[7]), "MALI %s %s",
                    shared->driver_metadata.driver_name.data(),
                    driver_version.c_str());
      std::snprintf(lines[8], sizeof(lines[8]), "WRAPPER %s-%s",
                    WRAPPER_VERSION, WRAPPER_GIT_REVISION);

      float widest_line = 0.0f;
      for (size_t line_index = 0; line_index < HUD_LINE_COUNT; ++line_index)
      {
         widest_line =
            std::max(widest_line,
                     measure_text(lines[line_index],
                                  HUD_LINE_FONT_HEIGHTS[line_index]));
      }
      const uint32_t content_width = static_cast<uint32_t>(
         std::ceil(widest_line + 24.0f * scale));
      const HudPanelLayout maximum_layout{
         panel.offset.x, panel.offset.y, panel.extent.width,
         panel.extent.height, scale
      };
      const HudPanelLayout content_layout =
         fit_panel_to_content(maximum_layout, content_width, config.position);
      const float content_right =
         static_cast<float>(content_layout.x) + content_layout.width -
         10.0f * scale;
      add_quad(vertices, count, static_cast<float>(content_layout.x),
               static_cast<float>(content_layout.y),
               static_cast<float>(content_layout.x + content_layout.width),
               static_cast<float>(content_layout.y + content_layout.height),
               0.0f, 0.0f, 0.0f, 0.0f, 0.025f, 0.035f, 0.055f,
               config.opacity);

      const float x = static_cast<float>(content_layout.x) + 12.0f * scale;
      for (size_t line_index = 0; line_index < HUD_LINE_COUNT; ++line_index)
      {
         const float baseline =
            static_cast<float>(content_layout.y) +
            HUD_LINE_BASELINES[line_index] * scale;
         add_text(vertices, count, lines[line_index], x, baseline,
                  content_right, HUD_LINE_FONT_HEIGHTS[line_index]);
      }

      auto *draw = reinterpret_cast<VkDrawIndirectCommand *>(
         static_cast<unsigned char *>(image.mapped) + indirect_offset);
      *draw = { count, 1, 0, 0 };
      if (HudRuntime::instance().config().debug)
      {
         static std::atomic<bool> draw_logged{ false };
         bool expected = false;
         if (draw_logged.compare_exchange_strong(expected, true, std::memory_order_relaxed))
         {
            std::fprintf(stderr,
                         "mali-hud: draw prepared (%u vertices, panel %d,%d %ux%u, "
                         "effective scale %.2f)\n",
                         count, content_layout.x, content_layout.y,
                         content_layout.width, content_layout.height, scale);
         }
      }
      if (!image.coherent)
      {
         VkMappedMemoryRange range{};
         range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
         range.memory = image.memory;
         range.offset = 0;
         range.size = VK_WHOLE_SIZE;
         return device.disp.FlushMappedMemoryRanges(device.device, 1, &range) == VK_SUCCESS;
      }
      return true;
   }

   VkCommandBuffer prepare(uint32_t image_index) noexcept
   {
      if (!enabled || image_index >= images.size())
         return VK_NULL_HANDLE;
      HudRuntime &runtime = HudRuntime::instance();
      runtime.present(runtime_id);
      if (!runtime.is_primary(runtime_id))
         return VK_NULL_HANDLE;
      uint64_t generation = 0;
      const MetricsSnapshot snapshot = runtime.metrics(&generation);
      ImageResources &image = images[image_index];
      if (last_generation[image_index] != generation)
      {
         if (!rebuild(image, snapshot))
         {
            enabled = false;
            runtime.log_disable_once("mapped HUD buffer update failed");
            return VK_NULL_HANDLE;
         }
         last_generation[image_index] = generation;
      }
      if (HudRuntime::instance().config().debug)
      {
         static std::atomic<bool> submitted_logged{ false };
         bool expected = false;
         if (submitted_logged.compare_exchange_strong(expected, true, std::memory_order_relaxed))
         {
            std::fprintf(stderr, "mali-hud: first overlay command submitted\n");
         }
      }
      return image.command;
   }

   device_private_data &device;
   VkQueue queue;
   VkSwapchainCreateInfoKHR create_info;
   const VkAllocationCallbacks *allocation_callbacks;
   std::shared_ptr<HudDeviceResources> shared;
   std::vector<ImageResources> images;
   std::vector<uint64_t> last_generation;
   VkRenderPass render_pass{ VK_NULL_HANDLE };
   VkPipeline pipeline{ VK_NULL_HANDLE };
   VkCommandPool command_pool{ VK_NULL_HANDLE };
   VkDeviceSize indirect_offset{ 0 };
   VkRect2D panel{};
   float scale{ 1.0f };
   float text_opacity{ 0.90f };
   uint64_t runtime_id{ 0 };
   bool enabled{ true };
   ApplicationMetadata application{};
   std::array<char, HUD_TEXT_SHORT> display_server{};
   std::string api_stack;
   std::string driver_version;
};

bool HudSwapchainResources::can_attach(device_private_data &device, const VkSwapchainCreateInfoKHR &create_info,
                                       bool headless) noexcept
{
   try
   {
      if (!HudRuntime::instance().enabled() || headless || create_info.imageArrayLayers != 1 ||
          create_info.imageExtent.width == 0 || create_info.imageExtent.height == 0 ||
          (create_info.imageFormat != VK_FORMAT_B8G8R8A8_UNORM &&
           create_info.imageFormat != VK_FORMAT_R8G8B8A8_UNORM))
      {
         return false;
      }
      auto format_properties = device.instance_data.disp.get_fn<PFN_vkGetPhysicalDeviceFormatProperties>(
         "vkGetPhysicalDeviceFormatProperties");
      auto queue_properties = device.instance_data.disp.get_fn<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
         "vkGetPhysicalDeviceQueueFamilyProperties");
      if (!format_properties || !queue_properties)
         return false;
      VkFormatProperties format{};
      (*format_properties)(device.physical_device, create_info.imageFormat, &format);
      if ((format.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) == 0)
         return false;
      VkImageFormatProperties image_format{};
      const VkResult image_support = device.instance_data.disp.GetPhysicalDeviceImageFormatProperties(
         device.physical_device, create_info.imageFormat, VK_IMAGE_TYPE_2D,
         VK_IMAGE_TILING_OPTIMAL, create_info.imageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
         0, &image_format);
      if (image_support != VK_SUCCESS)
         return false;
      uint32_t count = 0;
      (*queue_properties)(device.physical_device, &count, nullptr);
      // This wrapper presents through family zero and does not retain the
      // application's queue-family selection. Be conservative on devices with
      // multiple families rather than submit graphics work to an unknown one.
      if (count != 1)
         return false;
      VkQueueFamilyProperties queue{};
      uint32_t one = 1;
      (*queue_properties)(device.physical_device, &one, &queue);
      const bool supported = (queue.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
      if (supported && HudRuntime::instance().config().debug)
      {
         std::fprintf(stderr, "mali-hud: compatible wrapper-owned swapchain detected\n");
      }
      return supported;
   }
   catch (...)
   {
      HudRuntime::instance().log_disable_once("capability check failed");
      return false;
   }
}

std::unique_ptr<HudSwapchainResources>
HudSwapchainResources::create(device_private_data &device, VkQueue queue,
                              const VkSwapchainCreateInfoKHR &create_info, const VkImage *images,
                              uint32_t image_count, const char *display_server,
                              const VkAllocationCallbacks *allocator) noexcept
{
   try
   {
      if (queue == VK_NULL_HANDLE || images == nullptr || image_count == 0)
      {
         HudRuntime::instance().log_disable_once("invalid HUD queue or swapchain images");
         return {};
      }
      auto impl =
         std::make_unique<Impl>(device, queue, create_info, display_server,
                                allocator);
      if (!impl->initialize(images, image_count))
      {
         HudRuntime::instance().log_disable_once("swapchain renderer initialization failed");
         return {};
      }
      return std::unique_ptr<HudSwapchainResources>(new HudSwapchainResources(std::move(impl)));
   }
   catch (...)
   {
      HudRuntime::instance().log_disable_once("swapchain renderer initialization threw an exception");
      return {};
   }
}

HudSwapchainResources::HudSwapchainResources(std::unique_ptr<Impl> impl)
   : implementation(std::move(impl))
{
   implementation->last_generation.assign(implementation->images.size(), std::numeric_limits<uint64_t>::max());
}

HudSwapchainResources::~HudSwapchainResources() = default;

VkCommandBuffer HudSwapchainResources::prepare(uint32_t image_index) noexcept
{
   try
   {
      return implementation ? implementation->prepare(image_index) : VK_NULL_HANDLE;
   }
   catch (...)
   {
      if (implementation)
         implementation->enabled = false;
      HudRuntime::instance().log_disable_once("present preparation threw an exception");
      return VK_NULL_HANDLE;
   }
}

} // namespace mali_wrapper::hud
