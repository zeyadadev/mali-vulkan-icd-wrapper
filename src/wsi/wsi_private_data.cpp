/*
 * Copyright (c) 2018-2025 Arm Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <vulkan/vulkan.h>

#include "wsi_private_data.hpp"
#include "wsi_factory.hpp"
#include "wsi/surface.hpp"
#include "layer_utils/unordered_map.hpp"
#include "utils/logging.hpp"
#include "layer_utils/helpers.hpp"
#include "layer_utils/macros.hpp"
#include <cstdio>
#include <stdexcept>
#include <type_traits>
#include <pthread.h>

namespace mali_wrapper
{

static std::mutex g_data_lock;

static void log_mutex_state(const char *tag, std::mutex &mutex_ref)
{
   // Mutex logging disabled to reduce noise
}

/* The dictionaries below use plain pointers to store the instance/device private data objects.
 * This means that these objects are leaked if the application terminates without calling vkDestroyInstance
 * or vkDestroyDevice. This is fine as it is the application's responsibility to call these.
 */
static util::unordered_map<void *, mali_wrapper::instance_private_data *> g_instance_data{ util::allocator::get_generic() };
static util::unordered_map<void *, mali_wrapper::device_private_data *> g_device_data{ util::allocator::get_generic() };
static util::unordered_map<void *, void *> g_instance_key_mapping{ util::allocator::get_generic() };
// Device handle corruption fix: map device handles to their original WSI keys
static util::unordered_map<void *, void *> g_device_key_mapping{ util::allocator::get_generic() };
static util::unordered_map<void *, void *> g_queue_key_mapping{ util::allocator::get_generic() };

VkResult instance_dispatch_table::populate(VkInstance instance, PFN_vkGetInstanceProcAddr get_proc)
{
   static constexpr entrypoint entrypoints_init[] = {
#define DISPATCH_TABLE_ENTRY(name, ext_name, api_version, required) \
   { "vk" #name, ext_name, nullptr, api_version, false, required },
      INSTANCE_ENTRYPOINTS_LIST(DISPATCH_TABLE_ENTRY)
#undef DISPATCH_TABLE_ENTRY
   };

   static constexpr auto num_entrypoints = std::distance(std::begin(entrypoints_init), std::end(entrypoints_init));

   for (size_t i = 0; i < num_entrypoints; i++)
   {
      const entrypoint *entrypoint = &entrypoints_init[i];
      PFN_vkVoidFunction ret = get_proc(instance, entrypoint->name);
      if (!ret && entrypoint->required)
      {
         return VK_ERROR_INITIALIZATION_FAILED;
      }
      struct entrypoint e = *entrypoint;
      e.fn = ret;
      e.user_visible = false;

      if (!m_entrypoints->try_insert(std::make_pair(e.name, e)).has_value())
      {
         WSI_LOG_ERROR("Failed to allocate memory for instance dispatch table entry.");
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }

   return VK_SUCCESS;
}

void dispatch_table::set_user_enabled_extensions(const char *const *extension_names, size_t extension_count)
{
   for (size_t i = 0; i < extension_count; i++)
   {
      for (auto &entrypoint : *m_entrypoints)
      {
         if (!strcmp(entrypoint.second.ext_name, extension_names[i]))
         {
            entrypoint.second.user_visible = true;
         }
      }
   }
}

PFN_vkVoidFunction instance_dispatch_table::get_user_enabled_entrypoint(VkInstance instance, uint32_t api_version,
                                                                        const char *fn_name) const
{
   auto item = m_entrypoints->find(fn_name);
   if (item != m_entrypoints->end())
   {
      /* An entrypoint is allowed to use if it has been enabled by the user or is included in the core specficiation of the API version.
       * Entrypoints included in API version 1.0 are allowed by default. */
      if (item->second.user_visible || item->second.api_version <= api_version ||
          item->second.api_version == VK_API_VERSION_1_0)
      {
         return item->second.fn;
      }
      else
      {
         return nullptr;
      }
   }

   return GetInstanceProcAddr(instance, fn_name).value_or(nullptr);
}

VkResult device_dispatch_table::populate(VkDevice dev, PFN_vkGetDeviceProcAddr get_proc_fn)
{
   static constexpr entrypoint entrypoints_init[] = {
#define DISPATCH_TABLE_ENTRY(name, ext_name, api_version, required) \
   { "vk" #name, ext_name, nullptr, api_version, false, required },
      DEVICE_ENTRYPOINTS_LIST(DISPATCH_TABLE_ENTRY)
#undef DISPATCH_TABLE_ENTRY
   };
   static constexpr auto num_entrypoints = std::distance(std::begin(entrypoints_init), std::end(entrypoints_init));

   for (size_t i = 0; i < num_entrypoints; i++)
   {
      const entrypoint entrypoint = entrypoints_init[i];
      PFN_vkVoidFunction ret = get_proc_fn(dev, entrypoint.name);
      if (!ret && entrypoint.required)
      {
         return VK_ERROR_INITIALIZATION_FAILED;
      }
      struct entrypoint e = entrypoint;
      e.fn = ret;
      e.user_visible = false;

      if (!m_entrypoints->try_insert(std::make_pair(e.name, e)).has_value())
      {
         WSI_LOG_ERROR("Failed to allocate memory for device dispatch table entry.");
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }

   return VK_SUCCESS;
}

PFN_vkVoidFunction device_dispatch_table::get_user_enabled_entrypoint(VkDevice device, uint32_t api_version,
                                                                      const char *fn_name) const
{
   auto item = m_entrypoints->find(fn_name);
   if (item != m_entrypoints->end())
   {
      /* An entrypoint is allowed to use if it has been enabled by the user or is included in the core specficiation of the API version.
       * Entrypoints included in API version 1.0 are allowed by default. */
      if (item->second.user_visible || item->second.api_version <= api_version ||
          item->second.api_version == VK_API_VERSION_1_0)
      {
         return item->second.fn;
      }
      else
      {
         return nullptr;
      }
   }

   return GetDeviceProcAddr(device, fn_name).value_or(nullptr);
}

instance_private_data::instance_private_data(VkInstance instance, instance_dispatch_table table, PFN_vkSetInstanceLoaderData set_loader_data,
                                             util::wsi_platform_set enabled_layer_platforms, const uint32_t api_version,
                                             const util::allocator &alloc)
   : disp{ std::move(table) }
   , api_version{ api_version }
   , SetInstanceLoaderData{ set_loader_data }
   , enabled_layer_platforms{ enabled_layer_platforms }
   , allocator{ alloc }
   , m_instance{ instance }
   , surfaces{ alloc }
   , enabled_extensions{ allocator }
{
}

/**
 * @brief Obtain the loader's dispatch table for the given dispatchable object.
 * @note Dispatchable objects are structures that have a VkLayerDispatchTable as their first member.
         We treat the dispatchable object as a void** and then dereference to use the VkLayerDispatchTable
         as the key.
 */
template <typename dispatchable_type>
static inline void *get_key(dispatchable_type dispatchable_object)
{
   return *reinterpret_cast<void **>(dispatchable_object);
}

void register_queue_key_mapping(VkDevice device, VkQueue queue)
{
   if (queue == VK_NULL_HANDLE)
   {
      return;
   }

   scoped_mutex lock(g_data_lock);

   void *device_key = nullptr;
   auto device_key_it = g_device_key_mapping.find(static_cast<void *>(device));
   if (device_key_it != g_device_key_mapping.end())
   {
      device_key = device_key_it->second;
   }
   else
   {
      device_key = get_key(device);
   }

   if (device_key == nullptr)
   {
      WSI_LOG_WARNING("Failed to determine device key for queue mapping (device %p)", static_cast<void *>(device));
      return;
   }

   auto insert = g_queue_key_mapping.try_insert(
      std::make_pair(static_cast<void *>(queue), device_key));

   if (insert.has_value())
   {
      insert->first->second = device_key;
   }
   else
   {
      WSI_LOG_WARNING("Failed to store queue-to-key mapping for queue (%p)", static_cast<void *>(queue));
   }
}

VkResult instance_private_data::associate(VkInstance instance, instance_dispatch_table table,
                                          PFN_vkSetInstanceLoaderData set_loader_data,
                                          util::wsi_platform_set enabled_layer_platforms, const uint32_t api_version,
                                          const util::allocator &allocator)
{
   const auto key = get_key(instance);
   scoped_mutex lock(g_data_lock);

   // CRITICAL FIX: Check if same Mali instance already exists (Box64+Wine-Wow64+DXVK)
   // Compare dispatch table pointers to detect same Mali instance with different keys
   void* dispatch_table_ptr = *reinterpret_cast<void**>(instance);
   for (auto& pair : g_instance_data) {
      if (pair.second) {
         void* existing_dispatch = *reinterpret_cast<void**>(pair.second->get_instance_handle());
         if (existing_dispatch == dispatch_table_ptr) {
            WSI_LOG_WARNING("associate: Found same Mali instance %p (dispatch %p) with different key - reusing existing instance_data %p",
                           static_cast<void*>(instance), dispatch_table_ptr, static_cast<void*>(pair.second));

            // Add mapping from new key to existing instance_data
            auto result = g_instance_data.try_insert(std::make_pair(key, pair.second));
            if (result.has_value()) {
               // Update key mapping for this instance
               auto mapping_result = g_instance_key_mapping.try_insert(std::make_pair(static_cast<void *>(instance), key));
               if (!mapping_result.has_value()) {
                  mapping_result->first->second = key;
               }
            }

            return VK_SUCCESS;
         }
      }
   }

   auto instance_data = allocator.make_unique<instance_private_data>(instance, std::move(table), set_loader_data,
                                                                     enabled_layer_platforms, api_version, allocator);

   if (instance_data == nullptr)
   {
      WSI_LOG_ERROR("Instance private data for instance(%p) could not be allocated. Out of memory.",
                    reinterpret_cast<void *>(instance));
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   auto it = g_instance_data.find(key);
   if (it != g_instance_data.end())
   {
      WSI_LOG_WARNING("Hash collision when adding new instance (%p)", reinterpret_cast<void *>(instance));

      destroy(it->second);
      g_instance_data.erase(it);
   }

   auto result = g_instance_data.try_insert(std::make_pair(key, instance_data.get()));
   if (!result.has_value())
   {
      WSI_LOG_WARNING("Failed to insert instance_private_data for instance (%p) as host is out of memory",
                      reinterpret_cast<void *>(instance));
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   auto mapping_result = g_instance_key_mapping.try_insert(std::make_pair(static_cast<void *>(instance), key));
   if (!mapping_result.has_value())
   {
      mapping_result->first->second = key;
   }

   instance_data.release(); // NOLINT(bugprone-unused-return-value)
   return VK_SUCCESS;
}

void instance_private_data::disassociate(VkInstance instance)
{
   assert(instance != VK_NULL_HANDLE);
   instance_private_data *instance_data = nullptr;
   {
      scoped_mutex lock(g_data_lock);
      void *lookup_key = get_key(instance);
      auto key_it = g_instance_key_mapping.find(static_cast<void *>(instance));
      if (key_it != g_instance_key_mapping.end())
      {
         lookup_key = key_it->second;
         g_instance_key_mapping.erase(key_it);
      }

      auto it = g_instance_data.find(lookup_key);
      if (it == g_instance_data.end())
      {
         WSI_LOG_WARNING("Failed to find private data for instance (%p)", reinterpret_cast<void *>(instance));
         return;
      }

      instance_data = it->second;
      g_instance_data.erase(it);
   }

   destroy(instance_data);
}

template <typename dispatchable_type>
static instance_private_data &get_instance_private_data(dispatchable_type dispatchable_object)
{
   scoped_mutex lock(g_data_lock);
   void *lookup_key = get_key(dispatchable_object);
   auto map_it = g_instance_key_mapping.find(static_cast<void *>(dispatchable_object));
   if (map_it != g_instance_key_mapping.end())
   {
      lookup_key = map_it->second;
   }

   auto it = g_instance_data.find(lookup_key);
   if (it == g_instance_data.end())
   {
      throw std::out_of_range("Instance not found in WSI tracking map");
   }

   return *it->second;
}

instance_private_data &instance_private_data::get(VkInstance instance)
{
   try {
      return get_instance_private_data(instance);
   } catch (const std::out_of_range&) {
      // CRITICAL FIX: If not found by dispatch table key, check if same VkInstance exists with different key
      scoped_mutex lock(g_data_lock);
      for (auto& pair : g_instance_data) {
         if (pair.second && pair.second->get_instance_handle() == instance) {
            WSI_LOG_WARNING("get: Found same VkInstance %p with different key - reusing instance_data %p",
                           static_cast<void*>(instance), static_cast<void*>(pair.second));
            return *pair.second;
         }
      }
      throw; // Re-throw original exception if not found
   }
}

instance_private_data *instance_private_data::try_get(VkInstance instance)
{
   scoped_mutex lock(g_data_lock);
   void *lookup_key = get_key(instance);
   auto map_it = g_instance_key_mapping.find(static_cast<void *>(instance));
   if (map_it != g_instance_key_mapping.end())
   {
      lookup_key = map_it->second;
   }

   auto it = g_instance_data.find(lookup_key);
   if (it == g_instance_data.end())
   {
      // CRITICAL FIX: Check if same VkInstance exists with different key (Box64+Wine-Wow64+DXVK)
      for (auto& pair : g_instance_data) {
         if (pair.second && pair.second->get_instance_handle() == instance) {
            WSI_LOG_WARNING("try_get: Found same VkInstance %p with different key - reusing instance_data %p",
                           static_cast<void*>(instance), static_cast<void*>(pair.second));
            return pair.second;
         }
      }
      return nullptr;
   }
   return it->second;
}

instance_private_data &instance_private_data::get(VkPhysicalDevice phys_dev)
{
   return get_instance_private_data(phys_dev);
}

VkResult instance_private_data::add_surface(VkSurfaceKHR vk_surface, util::unique_ptr<wsi::surface> &wsi_surface)
{
   scoped_mutex lock(surfaces_lock);

   auto it = surfaces.find(vk_surface);
   if (it != surfaces.end())
   {
      WSI_LOG_WARNING("Hash collision when adding new surface (%p). Old surface is replaced.",
                      reinterpret_cast<void *>(vk_surface));
      surfaces.erase(it);
   }

   auto result = surfaces.try_insert(std::make_pair(vk_surface, nullptr));
   if (result.has_value())
   {
      assert(result->second);
      result->first->second = wsi_surface.release();
      return VK_SUCCESS;
   }

   return VK_ERROR_OUT_OF_HOST_MEMORY;
}

wsi::surface *instance_private_data::get_surface(VkSurfaceKHR vk_surface)
{
   scoped_mutex lock(surfaces_lock);
   auto it = surfaces.find(vk_surface);
   if (it != surfaces.end())
   {
      return it->second;
   }

   return nullptr;
}

void instance_private_data::remove_surface(VkSurfaceKHR vk_surface, const util::allocator &alloc)
{
   scoped_mutex lock(surfaces_lock);
   auto it = surfaces.find(vk_surface);
   if (it != surfaces.end())
   {
      alloc.destroy<wsi::surface>(1, it->second);
      surfaces.erase(it);
   }
   /* Failing to find a surface is not an error. It could have been created by a WSI extension, which is not handled
    * by this layer.
    */
}

bool instance_private_data::does_layer_support_surface(VkSurfaceKHR surface)
{
   scoped_mutex lock(surfaces_lock);
   auto it = surfaces.find(surface);
   bool found = it != surfaces.end();
   
   return found;
}

void instance_private_data::destroy(instance_private_data *instance_data)
{
   assert(instance_data);

   auto alloc = instance_data->get_allocator();
   alloc.destroy<instance_private_data>(1, instance_data);
}

bool instance_private_data::do_icds_support_surface(VkPhysicalDevice, VkSurfaceKHR)
{
   /* For now assume ICDs do not support VK_KHR_surface. This means that the layer will handle all the surfaces it can
    * handle (even if the ICDs can handle the surface) and only call down for surfaces it cannot handle. In the future
    * we may allow system integrators to configure which ICDs have precedence handling which platforms.
    */
   return false;
}

bool instance_private_data::should_layer_handle_surface(VkPhysicalDevice phys_dev, VkSurfaceKHR surface)
{
   /* If the layer cannot handle the surface, then necessarily the ICDs or layers below us must be able to do it:
    * the fact that the surface exists means that the Vulkan loader created it. In turn, this means that someone
    * among the ICDs and layers advertised support for it. If it's not us, then it must be one of the layers/ICDs
    * below us. It is therefore safe to always return false (and therefore call-down) when layer_can_handle_surface
    * is false.
    */
   bool icd_can_handle_surface = do_icds_support_surface(phys_dev, surface);
   bool layer_can_handle_surface = does_layer_support_surface(surface);
   bool ret = layer_can_handle_surface && !icd_can_handle_surface;

   return ret;
}

bool instance_private_data::has_image_compression_support(VkPhysicalDevice phys_dev)
{
   VkPhysicalDeviceImageCompressionControlFeaturesEXT compression = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_FEATURES_EXT, nullptr, VK_FALSE
   };
   VkPhysicalDeviceFeatures2KHR features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR, &compression, {} };

   disp.GetPhysicalDeviceFeatures2KHR(phys_dev, &features);

   return compression.imageCompressionControl != VK_FALSE;
}

bool instance_private_data::has_frame_boundary_support(VkPhysicalDevice phys_dev)
{
   VkPhysicalDeviceFrameBoundaryFeaturesEXT frame_boundary = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_FEATURES_EXT, nullptr, VK_FALSE
   };
   VkPhysicalDeviceFeatures2KHR features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR, &frame_boundary, {} };

   disp.GetPhysicalDeviceFeatures2KHR(phys_dev, &features);

   return frame_boundary.frameBoundary != VK_FALSE;
}

VkResult instance_private_data::set_instance_enabled_extensions(const char *const *extension_names,
                                                                size_t extension_count)
{
   return enabled_extensions.add(extension_names, extension_count);
}

bool instance_private_data::is_instance_extension_enabled(const char *extension_name) const
{
   return enabled_extensions.contains(extension_name);
}

void instance_private_data::set_maintainance1_support(bool enabled_unsupport_ext)
{
   enabled_unsupported_swapchain_maintenance1_extensions = enabled_unsupport_ext;
}

bool instance_private_data::get_maintainance1_support()
{
   return enabled_unsupported_swapchain_maintenance1_extensions;
}

device_private_data::device_private_data(instance_private_data &inst_data, VkPhysicalDevice phys_dev, VkDevice dev,
                                         device_dispatch_table table, PFN_vkSetDeviceLoaderData set_loader_data,
                                         const util::allocator &alloc)
   : disp{ std::move(table) }
   , instance_data{ inst_data }
   , SetDeviceLoaderData{ set_loader_data }
   , physical_device{ phys_dev }
   , device{ dev }
   , allocator{ alloc }
   , swapchains{ allocator } /* clang-format off */
   , enabled_extensions{ allocator }
   , compression_control_enabled{ false }
   , present_id_enabled { false }
   , swapchain_maintenance1_enabled{ false }
#if VULKAN_WSI_LAYER_EXPERIMENTAL
   , present_timing_enabled { true }
#endif
/* clang-format on */
{
}

VkResult device_private_data::associate(VkDevice dev, instance_private_data &inst_data, VkPhysicalDevice phys_dev,
                                        device_dispatch_table table, PFN_vkSetDeviceLoaderData set_loader_data,
                                        const util::allocator &allocator)
{
   auto device_data = allocator.make_unique<device_private_data>(inst_data, phys_dev, dev, std::move(table),
                                                                 set_loader_data, allocator);
   WSI_LOG_DEBUG("device_private_data::associate device=%p instance_data=%p", reinterpret_cast<void *>(dev), (void*)&inst_data);

   if (device_data == nullptr)
   {
      WSI_LOG_ERROR("Device private data for device(%p) could not be allocated. Out of memory.",
                    reinterpret_cast<void *>(dev));
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   const auto dispatch_key = get_key(dev);
   void *store_key = dispatch_key;
   scoped_mutex lock(g_data_lock);

   auto key_mapping_it = g_device_key_mapping.find(static_cast<void *>(dev));
   if (key_mapping_it != g_device_key_mapping.end())
   {
      store_key = key_mapping_it->second;
      auto existing = g_device_data.find(store_key);
      if (existing != g_device_data.end())
      {
         WSI_LOG_WARNING("Replacing existing device_private_data for device (%p)", reinterpret_cast<void *>(dev));
         destroy(existing->second);
         g_device_data.erase(existing);
      }
   }
   else
   {
      auto collision = g_device_data.find(dispatch_key);
      if (collision != g_device_data.end())
      {
         if (collision->second != nullptr && collision->second->device == dev)
         {
            WSI_LOG_WARNING("Replacing existing device_private_data for device (%p)", reinterpret_cast<void *>(dev));
            destroy(collision->second);
            g_device_data.erase(collision);
         }
         else
         {
            WSI_LOG_WARNING("Dispatch key collision: device (%p) shares dispatch table with device (%p)",
                            reinterpret_cast<void *>(dev),
                            (collision->second != nullptr) ? reinterpret_cast<void *>(collision->second->device) : nullptr);
            store_key = device_data.get();
         }
      }
   }

   auto result = g_device_data.try_insert(std::make_pair(store_key, device_data.get()));
   if (!result.has_value())
   {
      WSI_LOG_WARNING("Failed to insert device_private_data for device (%p) as host is out of memory",
                      reinterpret_cast<void *>(dev));
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   auto mapping_result = g_device_key_mapping.try_insert(std::make_pair(static_cast<void *>(dev), store_key));
   if (!mapping_result.has_value())
   {
      mapping_result->first->second = store_key;
   }

   device_data.release();
   return VK_SUCCESS;
}

void device_private_data::disassociate(VkDevice dev)
{
   assert(dev != VK_NULL_HANDLE);
   device_private_data *device_data = nullptr;
   void *stored_device_key = nullptr;
   {
      scoped_mutex lock(g_data_lock);

      void *lookup_key = get_key(dev);
      auto key_it = g_device_key_mapping.find(static_cast<void *>(dev));
      if (key_it != g_device_key_mapping.end())
      {
         lookup_key = key_it->second;
         stored_device_key = key_it->second;
         g_device_key_mapping.erase(key_it);
      }
      else
      {
         stored_device_key = lookup_key;
      }

      auto it = g_device_data.find(lookup_key);
      if (it == g_device_data.end())
      {
         WSI_LOG_WARNING("Failed to find private data for device (%p)", reinterpret_cast<void *>(dev));
         return;
      }

      device_data = it->second;
      g_device_data.erase(it);

      if (stored_device_key != nullptr)
      {
         for (auto queue_it = g_queue_key_mapping.begin(); queue_it != g_queue_key_mapping.end();)
         {
            if (queue_it->second == stored_device_key)
            {
               queue_it = g_queue_key_mapping.erase(queue_it);
            }
            else
            {
               ++queue_it;
            }
         }
      }
   }

   destroy(device_data);
}

template <typename dispatchable_type>
static device_private_data &get_device_private_data(dispatchable_type dispatchable_object)
{
   scoped_mutex lock(g_data_lock);

   void* device_handle = static_cast<void*>(dispatchable_object);
   void* original_key = nullptr;

   // First try queue mapping (for queues associated after loader data calls)
   auto queue_mapping_it = g_queue_key_mapping.find(device_handle);
   if (queue_mapping_it != g_queue_key_mapping.end())
   {
      original_key = queue_mapping_it->second;
   }
   else
   {
      // Then try to get the stored key from our device mapping (corruption fix)
      auto key_mapping_it = g_device_key_mapping.find(device_handle);
      if (key_mapping_it != g_device_key_mapping.end())
      {
         original_key = key_mapping_it->second;
      }
      else
      {
         // Fallback to dereferencing (may be corrupted)
         original_key = get_key(dispatchable_object);
      }
   }

   auto it = g_device_data.find(original_key);
   if (it == g_device_data.end())
   {
      if constexpr (std::is_same_v<dispatchable_type, VkDevice>)
      {
         for (auto &entry : g_device_data)
         {
            if (entry.second != nullptr && entry.second->device == dispatchable_object)
            {
               WSI_LOG_WARNING("Recovered device mapping for %p via handle fallback", device_handle);
               auto mapping_insert = g_device_key_mapping.try_insert(std::make_pair(device_handle, entry.first));
               if (!mapping_insert.has_value())
               {
                  mapping_insert->first->second = entry.first;
               }
               it = g_device_data.find(entry.first);
               break;
            }
         }
      }

      if (it == g_device_data.end())
      {
         throw std::out_of_range("Device not found in WSI tracking map");
      }
   }

   // Device lookup logging disabled to reduce noise
   return *it->second;
}

device_private_data &device_private_data::get(VkDevice device)
{
   return get_device_private_data(device);
}

device_private_data *device_private_data::try_get(VkDevice device)
{
   scoped_mutex lock(g_data_lock);
   auto key_it = g_device_key_mapping.find(static_cast<void *>(device));
   void *lookup_key = nullptr;
   if (key_it != g_device_key_mapping.end())
   {
      lookup_key = key_it->second;
   }
   else
   {
      lookup_key = get_key(device);
   }

   auto it = g_device_data.find(lookup_key);
   if (it == g_device_data.end())
   {
      return nullptr;
   }
   return it->second;
}

device_private_data &device_private_data::get(VkQueue queue)
{
   return get_device_private_data(queue);
}

VkResult device_private_data::add_layer_swapchain(VkSwapchainKHR swapchain)
{
   scoped_mutex lock(swapchains_lock);
   auto result = swapchains.try_insert(swapchain);
   return result.has_value() ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

void device_private_data::remove_layer_swapchain(VkSwapchainKHR swapchain)
{
   scoped_mutex lock(swapchains_lock);
   auto it = swapchains.find(swapchain);
   if (it != swapchains.end())
   {
      swapchains.erase(swapchain);
   }
}

bool device_private_data::layer_owns_all_swapchains(const VkSwapchainKHR *swapchain, uint32_t swapchain_count) const
{
   scoped_mutex lock(swapchains_lock);
   for (uint32_t i = 0; i < swapchain_count; i++)
   {
      if (swapchains.find(swapchain[i]) == swapchains.end())
      {
         return false;
      }
   }
   return true;
}

bool device_private_data::should_layer_create_swapchain(VkSurfaceKHR vk_surface)
{
   return instance_data.should_layer_handle_surface(physical_device, vk_surface);
}

bool device_private_data::can_icds_create_swapchain(VkSurfaceKHR vk_surface)
{
   UNUSED(vk_surface);
   // Mali drivers do not support WSI functions - always return false
   // to force the WSI layer to handle all swapchain operations
   WSI_LOG_DEBUG("Mali drivers don't support WSI - forcing WSI layer to handle swapchain");
   return false;
}

VkResult device_private_data::set_device_enabled_extensions(const char *const *extension_names, size_t extension_count)
{
   return enabled_extensions.add(extension_names, extension_count);
}

bool device_private_data::is_device_extension_enabled(const char *extension_name) const
{
   return enabled_extensions.contains(extension_name);
}

void device_private_data::destroy(device_private_data *device_data)
{
   assert(device_data);

   auto alloc = device_data->get_allocator();
   alloc.destroy<device_private_data>(1, device_data);
}

void device_private_data::set_swapchain_compression_control_enabled(bool enable)
{
   compression_control_enabled = enable;
}

bool device_private_data::is_swapchain_compression_control_enabled() const
{
   return compression_control_enabled;
}

void device_private_data::set_layer_frame_boundary_handling_enabled(bool enable)
{
   handle_frame_boundary_events = enable;
}

bool device_private_data::should_layer_handle_frame_boundary_events() const
{
   return handle_frame_boundary_events;
}

void device_private_data::set_present_id_feature_enabled(bool enable)
{
   present_id_enabled = enable;
}

bool device_private_data::is_present_id_enabled()
{
   return present_id_enabled;
}

void device_private_data::set_swapchain_maintenance1_enabled(bool enable)
{
   swapchain_maintenance1_enabled = enable;
}

bool device_private_data::is_swapchain_maintenance1_enabled() const
{
   return swapchain_maintenance1_enabled;
}

} /* namespace mali_wrapper */
