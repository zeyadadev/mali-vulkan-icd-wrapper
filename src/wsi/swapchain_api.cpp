/*
 * Copyright (c) 2017, 2019, 2021-2025 Arm Limited.
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

/**
 * @file swapchain_api.cpp
 *
 * @brief Contains the Vulkan entrypoints for the swapchain.
 */

#include <cassert>
#include <cstdlib>
#include <new>
#include <atomic>

#include "wsi/wsi_private_data.hpp"
#include "swapchain_api.hpp"

#include "layer_utils/helpers.hpp"

#include <wsi/synchronization.hpp>
#include <wsi/wsi_factory.hpp>
#include <wsi/extensions/frame_boundary.hpp>
#include "layer_utils/macros.hpp"

VWL_VKAPI_CALL(VkResult) __attribute__((visibility("default")))
wsi_layer_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pSwapchainCreateInfo,
                               const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain) VWL_API_POST
{
   WSI_LOG_DEBUG("vkCreateSwapchainKHR called with device=%p", (void*)device);
   assert(pSwapchain != nullptr);

   // Try to get device_data and catch the exception if device not found
   wsi::device_private_data *device_data_ptr = nullptr;
   try {
      device_data_ptr = &wsi::device_private_data::get(device);
      WSI_LOG_DEBUG("Device %p found in tracking map", (void*)device);
   } catch (const std::out_of_range& e) {
      WSI_LOG_ERROR("WSI device lookup failed for %p: %s", (void*)device, e.what());

      // For now, just fail - we need to investigate the root cause
      // The real fix should ensure the correct device handle is passed
      WSI_LOG_DEBUG("Device not found in WSI tracking");
      return VK_ERROR_INITIALIZATION_FAILED;
   } catch (const std::exception& e) {
      WSI_LOG_ERROR("Exception getting device data for %p: %s", (void*)device, e.what());
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   wsi::device_private_data &device_data = *device_data_ptr;
   VkSurfaceKHR surface = pSwapchainCreateInfo->surface;

   if (!device_data.should_layer_create_swapchain(surface))
   {
      WSI_LOG_DEBUG("Layer cannot create swapchain, checking ICD");
      if (!device_data.can_icds_create_swapchain(surface))
      {
         WSI_LOG_ERROR("Neither layer nor ICD can create swapchain");
         return VK_ERROR_INITIALIZATION_FAILED;
      }
      WSI_LOG_DEBUG("Forwarding swapchain creation to ICD");
      return device_data.disp.CreateSwapchainKHR(device_data.device, pSwapchainCreateInfo, pAllocator, pSwapchain);
   }
   WSI_LOG_DEBUG("Layer creating swapchain");

   auto sc = wsi::allocate_surface_swapchain(surface, device_data, pAllocator);
   if (sc == nullptr)
   {
      WSI_LOG_ERROR("Failed to allocate surface swapchain");
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   VkSwapchainCreateInfoKHR my_create_info = *pSwapchainCreateInfo;
   my_create_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
   VkResult init_result = sc->init(device_data.device, &my_create_info);
   WSI_LOG_DEBUG("swapchain init result %d", init_result);
   if (init_result != VK_SUCCESS)
   {
      WSI_LOG_ERROR("Failed to initialise swapchain, error %d", init_result);
      return init_result;
   }

   VkResult assoc_result = device_data.add_layer_swapchain(reinterpret_cast<VkSwapchainKHR>(sc.get()));
   WSI_LOG_DEBUG("add_layer_swapchain result %d", assoc_result);
   if (assoc_result != VK_SUCCESS)
   {
      WSI_LOG_ERROR("Failed to associate swapchain with the layer, error %d", assoc_result);
      return assoc_result;
   }

   *pSwapchain = reinterpret_cast<VkSwapchainKHR>(sc.release());
   WSI_LOG_DEBUG("vkCreateSwapchainKHR returning success with swapchain %p", (void*)*pSwapchain);
   return VK_SUCCESS;
}

VWL_VKAPI_CALL(void) __attribute__((visibility("default")))
wsi_layer_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapc,
                                const VkAllocationCallbacks *pAllocator) VWL_API_POST
{
   wsi::device_private_data &device_data = wsi::device_private_data::get(device);

   if (!device_data.layer_owns_swapchain(swapc))
   {
      return device_data.disp.DestroySwapchainKHR(device_data.device, swapc, pAllocator);
   }

   assert(swapc != VK_NULL_HANDLE);
   device_data.remove_layer_swapchain(swapc);

   auto *sc = reinterpret_cast<wsi::swapchain_base *>(swapc);
   wsi::destroy_surface_swapchain(sc, device_data, pAllocator);
}

VWL_VKAPI_CALL(VkResult) __attribute__((visibility("default")))
wsi_layer_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapc, uint32_t *pSwapchainImageCount,
                                  VkImage *pSwapchainImages) VWL_API_POST
{
   wsi::device_private_data &device_data = wsi::device_private_data::get(device);

   if (!device_data.layer_owns_swapchain(swapc))
   {
      return device_data.disp.GetSwapchainImagesKHR(device_data.device, swapc, pSwapchainImageCount, pSwapchainImages);
   }

   assert(pSwapchainImageCount != nullptr);
   assert(swapc != VK_NULL_HANDLE);
   auto *sc = reinterpret_cast<wsi::swapchain_base *>(swapc);
   return sc->get_swapchain_images(pSwapchainImageCount, pSwapchainImages);
}

VWL_VKAPI_CALL(VkResult) __attribute__((visibility("default")))
wsi_layer_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapc, uint64_t timeout, VkSemaphore semaphore,
                                VkFence fence, uint32_t *pImageIndex) VWL_API_POST
{
   wsi::device_private_data &device_data = wsi::device_private_data::get(device);

   if (!device_data.layer_owns_swapchain(swapc))
   {
      static std::atomic<bool> warned_forward_acquire{ false };
      bool expected = false;
      if (warned_forward_acquire.compare_exchange_strong(expected, true))
      {
         WSI_LOG_WARNING("vkAcquireNextImageKHR forwarded to ICD: swapchain=%p is not layer-owned", (void *)swapc);
      }
      return device_data.disp.AcquireNextImageKHR(device_data.device, swapc, timeout, semaphore, fence, pImageIndex);
   }

   assert(swapc != VK_NULL_HANDLE);
   assert(semaphore != VK_NULL_HANDLE || fence != VK_NULL_HANDLE);
   assert(pImageIndex != nullptr);
   auto *sc = reinterpret_cast<wsi::swapchain_base *>(swapc);
   VkResult res = sc->acquire_next_image(timeout, semaphore, fence, pImageIndex);
   if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
   {
      WSI_LOG_ERROR("vkAcquireNextImageKHR failed: result=%d swapchain=%p timeout=%llu", res, (void *)swapc,
                    static_cast<unsigned long long>(timeout));
   }
   return res;
}

static VkResult submit_wait_request(VkQueue queue, const VkPresentInfoKHR &present_info,
                                    wsi::device_private_data &device_data, bool &frame_boundary_event_handled)
{
   util::vector<VkSemaphore> swapchain_semaphores{ util::allocator(device_data.get_allocator(),
                                                                   VK_SYSTEM_ALLOCATION_SCOPE_COMMAND) };
   if (!swapchain_semaphores.try_resize(present_info.swapchainCount))
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   for (uint32_t i = 0; i < present_info.swapchainCount; ++i)
   {
      auto swapchain = reinterpret_cast<wsi::swapchain_base *>(present_info.pSwapchains[i]);
      swapchain_semaphores[i] = swapchain->get_image_present_semaphore(present_info.pImageIndices[i]);
   }

   wsi::queue_submit_semaphores semaphores = { present_info.pWaitSemaphores, present_info.waitSemaphoreCount,
                                               swapchain_semaphores.data(),
                                               static_cast<uint32_t>(swapchain_semaphores.size()) };

   void *submission_pnext = nullptr;
   auto frame_boundary = wsi::create_frame_boundary(present_info);
   if (frame_boundary.has_value())
   {
      submission_pnext = &frame_boundary.value();
   }

   /* Notify that we don't want to pass any further frame boundary events */
   frame_boundary_event_handled = submission_pnext != nullptr;

   TRY(wsi::sync_queue_submit(device_data, queue, VK_NULL_HANDLE, semaphores, submission_pnext));
   return VK_SUCCESS;
}

VWL_VKAPI_CALL(VkResult) __attribute__((visibility("default")))
wsi_layer_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo) VWL_API_POST
{
   assert(queue != VK_NULL_HANDLE);
   assert(pPresentInfo != nullptr);

   wsi::device_private_data &device_data = wsi::device_private_data::get(queue);

   if (!device_data.layer_owns_all_swapchains(pPresentInfo->pSwapchains, pPresentInfo->swapchainCount))
   {
      static std::atomic<bool> warned_forward_present{ false };
      bool expected = false;
      if (warned_forward_present.compare_exchange_strong(expected, true))
      {
         WSI_LOG_WARNING("vkQueuePresentKHR forwarded to ICD: at least one swapchain is not layer-owned");
      }
      return device_data.disp.QueuePresentKHR(queue, pPresentInfo);
   }

   /* Avoid allocating on the heap when there is only one swapchain. */
   const VkPresentInfoKHR *present_info = pPresentInfo;
   bool use_image_present_semaphore = false;
   bool frame_boundary_event_handled = true;
   if (pPresentInfo->swapchainCount > 1)
   {
      TRY_LOG_CALL(submit_wait_request(queue, *pPresentInfo, device_data, frame_boundary_event_handled));
      use_image_present_semaphore = true;
   }

   VkResult ret = VK_SUCCESS;

   auto *present_ids = util::find_extension<VkPresentIdKHR>(VK_STRUCTURE_TYPE_PRESENT_ID_KHR, pPresentInfo->pNext);
   const auto present_fence_info = util::find_extension<VkSwapchainPresentFenceInfoEXT>(
      VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT, present_info->pNext);
   const auto swapchain_present_mode_info = util::find_extension<VkSwapchainPresentModeInfoEXT>(
      VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT, present_info->pNext);
#if VULKAN_WSI_LAYER_EXPERIMENTAL
   const auto present_timings_info =
      util::find_extension<VkPresentTimingsInfoEXT>(VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT, present_info->pNext);
   if (present_timings_info)
   {
      assert(present_timings_info->swapchainCount == pPresentInfo->swapchainCount);
   }
#endif
   for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
   {
      VkSwapchainKHR swapc = pPresentInfo->pSwapchains[i];
      auto *sc = reinterpret_cast<wsi::swapchain_base *>(swapc);
      assert(sc != nullptr);

      uint64_t present_id = 0; /* No present ID by default */
      if (present_ids && present_ids->pPresentIds && present_ids->swapchainCount == pPresentInfo->swapchainCount)
      {
         present_id = present_ids->pPresentIds[i];
      }

      wsi::swapchain_presentation_parameters present_params{};
      present_params.present_fence = (present_fence_info == nullptr) ? VK_NULL_HANDLE : present_fence_info->pFences[i];
      if (swapchain_present_mode_info != nullptr)
      {
         present_params.switch_presentation_mode = true;
         present_params.present_mode = swapchain_present_mode_info->pPresentModes[i];
      }

      present_params.pending_present.image_index = pPresentInfo->pImageIndices[i];
      present_params.pending_present.present_id = present_id;

      present_params.use_image_present_semaphore = use_image_present_semaphore;
      present_params.handle_present_frame_boundary_event = frame_boundary_event_handled;

#if VULKAN_WSI_LAYER_EXPERIMENTAL
      if (present_timings_info)
      {
         present_params.m_present_timing_info = present_timings_info->pTimingInfos[i];
         present_params.m_present_timing_info.pNext = nullptr;
      }
#endif
      VkResult res = sc->queue_present(queue, present_info, present_params);
      if (pPresentInfo->pResults != nullptr)
      {
         pPresentInfo->pResults[i] = res;
      }

      if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
      {
         WSI_LOG_ERROR("vkQueuePresentKHR failed for swapchain[%u]=%p imageIndex=%u result=%d", i,
                       (void *)swapc, present_params.pending_present.image_index, res);
      }

      if (res != VK_SUCCESS && ret == VK_SUCCESS)
      {
         ret = res;
      }
   }

   return ret;
}

VWL_VKAPI_CALL(VkResult)
wsi_layer_vkGetDeviceGroupPresentCapabilitiesKHR(
   VkDevice device, VkDeviceGroupPresentCapabilitiesKHR *pDeviceGroupPresentCapabilities) VWL_API_POST
{
   UNUSED(device);
   assert(pDeviceGroupPresentCapabilities != nullptr);

   pDeviceGroupPresentCapabilities->presentMask[0] = 1;
   pDeviceGroupPresentCapabilities->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;

   for (uint32_t i = 1; i < VK_MAX_DEVICE_GROUP_SIZE_KHR; i++)
   {
      pDeviceGroupPresentCapabilities->presentMask[i] = 0;
   }

   return VK_SUCCESS;
}

VWL_VKAPI_CALL(VkResult)
wsi_layer_vkGetDeviceGroupSurfacePresentModesKHR(VkDevice device, VkSurfaceKHR surface,
                                                 VkDeviceGroupPresentModeFlagsKHR *pModes) VWL_API_POST
{
   assert(pModes != nullptr);

   auto &device_data = wsi::device_private_data::get(device);
   auto &instance = device_data.instance_data;

   if (!instance.should_layer_handle_surface(device_data.physical_device, surface))
   {
      return device_data.disp.GetDeviceGroupSurfacePresentModesKHR(device, surface, pModes);
   }

   *pModes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
   return VK_SUCCESS;
}

VWL_VKAPI_CALL(VkResult)
wsi_layer_vkGetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                                  uint32_t *pRectCount, VkRect2D *pRects) VWL_API_POST
{
   assert(surface);
   assert(pRectCount != nullptr);

   auto &instance = wsi::instance_private_data::get(physicalDevice);

   if (!instance.should_layer_handle_surface(physicalDevice, surface))
   {
      return instance.disp.GetPhysicalDevicePresentRectanglesKHR(physicalDevice, surface, pRectCount, pRects);
   }

   VkResult result;
   wsi::surface_properties *props = wsi::get_surface_properties(instance, surface);
   assert(props);

   if (nullptr == pRects)
   {
      *pRectCount = 1;
      result = VK_SUCCESS;
   }
   else if (0 == *pRectCount)
   {
      result = VK_INCOMPLETE;
   }
   else
   {
      *pRectCount = 1;

      VkSurfaceCapabilitiesKHR surface_caps = {};
      result = props->get_surface_capabilities(physicalDevice, &surface_caps);

      if (result != VK_SUCCESS)
      {
         return result;
      }

      pRects[0].offset.x = 0;
      pRects[0].offset.y = 0;
      pRects[0].extent = surface_caps.currentExtent;
   }

   return result;
}

VWL_VKAPI_CALL(VkResult) __attribute__((visibility("default")))
wsi_layer_vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *pAcquireInfo,
                                 uint32_t *pImageIndex) VWL_API_POST
{
   assert(pAcquireInfo != VK_NULL_HANDLE);
   assert(pAcquireInfo->swapchain != VK_NULL_HANDLE);
   assert(pAcquireInfo->semaphore != VK_NULL_HANDLE || pAcquireInfo->fence != VK_NULL_HANDLE);
   assert(pImageIndex != nullptr);

   auto &device_data = wsi::device_private_data::get(device);

   if (!device_data.layer_owns_swapchain(pAcquireInfo->swapchain))
   {
      static std::atomic<bool> warned_forward_acquire2{ false };
      bool expected = false;
      if (warned_forward_acquire2.compare_exchange_strong(expected, true))
      {
         WSI_LOG_WARNING("vkAcquireNextImage2KHR forwarded to ICD: swapchain=%p is not layer-owned",
                         (void *)pAcquireInfo->swapchain);
      }
      return device_data.disp.AcquireNextImage2KHR(device, pAcquireInfo, pImageIndex);
   }

   auto *sc = reinterpret_cast<wsi::swapchain_base *>(pAcquireInfo->swapchain);
   VkResult res =
      sc->acquire_next_image(pAcquireInfo->timeout, pAcquireInfo->semaphore, pAcquireInfo->fence, pImageIndex);
   if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
   {
      WSI_LOG_ERROR("vkAcquireNextImage2KHR failed: result=%d swapchain=%p timeout=%llu", res,
                    (void *)pAcquireInfo->swapchain, static_cast<unsigned long long>(pAcquireInfo->timeout));
   }
   return res;
}

VWL_VKAPI_CALL(VkResult)
wsi_layer_vkCreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
                        VkImage *pImage) VWL_API_POST
{
   auto &device_data = wsi::device_private_data::get(device);

   const auto *image_sc_create_info = util::find_extension<VkImageSwapchainCreateInfoKHR>(
      VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR, pCreateInfo->pNext);

   if (image_sc_create_info == nullptr || !device_data.layer_owns_swapchain(image_sc_create_info->swapchain))
   {
      return device_data.disp.CreateImage(device_data.device, pCreateInfo, pAllocator, pImage);
   }

   auto sc = reinterpret_cast<wsi::swapchain_base *>(image_sc_create_info->swapchain);
   return sc->create_aliased_image_handle(pImage);
}

VWL_VKAPI_CALL(VkResult)
wsi_layer_vkBindImageMemory2(VkDevice device, uint32_t bindInfoCount,
                             const VkBindImageMemoryInfo *pBindInfos) VWL_API_POST
{
   auto &device_data = wsi::device_private_data::get(device);

   VkResult endpoint_result = VK_SUCCESS;
   bool maintenance_6 = device_data.is_device_extension_enabled(VK_KHR_MAINTENANCE_6_EXTENSION_NAME);
   for (uint32_t i = 0; i < bindInfoCount; i++)
   {
      VkResult result = VK_SUCCESS;
      std::string_view error_message{};

      const auto *bind_sc_info = util::find_extension<VkBindImageMemorySwapchainInfoKHR>(
         VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR, pBindInfos[i].pNext);

      if (bind_sc_info == nullptr || bind_sc_info->swapchain == VK_NULL_HANDLE ||
          !device_data.layer_owns_swapchain(bind_sc_info->swapchain))
      {
         result = device_data.disp.BindImageMemory2KHR(device_data.device, 1, &pBindInfos[i]);
         error_message = "Failed to bind image memory";
      }
      else
      {
         auto sc = reinterpret_cast<wsi::swapchain_base *>(bind_sc_info->swapchain);
         TRY_LOG(sc->is_bind_allowed(bind_sc_info->imageIndex),
                 "Bind is not allowed on images that haven't been acquired first.");
         result = sc->bind_swapchain_image(device, &pBindInfos[i], bind_sc_info);
         error_message = "Failed to bind an image to the swapchain";
      }

      if (maintenance_6)
      {
         const auto *bind_status =
            util::find_extension<VkBindMemoryStatusKHR>(VK_STRUCTURE_TYPE_BIND_MEMORY_STATUS_KHR, pBindInfos[i].pNext);
         if (nullptr != bind_status)
         {
            assert(bind_status->pResult != nullptr);
            *bind_status->pResult = result;
         }
      }

      if (VK_SUCCESS != result)
      {
         /* VK_KHR_maintenance6 requires that all memory binding operations must be attempted, so the results are stored
         rather than returned early upon failure */
         WSI_LOG_ERROR("%s", error_message.data());
         endpoint_result = result;
      }
   }
   return endpoint_result;
}

VWL_VKAPI_CALL(VkResult) __attribute__((visibility("default")))
wsi_layer_vkGetSwapchainStatusKHR(VkDevice device, VkSwapchainKHR swapchain) VWL_API_POST
{
   auto &device_data = wsi::device_private_data::get(device);

   if (!device_data.layer_owns_swapchain(swapchain))
   {
      return device_data.disp.GetSwapchainStatusKHR(device, swapchain);
   }

   auto *sc = reinterpret_cast<wsi::swapchain_base *>(swapchain);

   return sc->get_swapchain_status();
}
