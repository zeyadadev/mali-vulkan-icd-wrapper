/*
 * Copyright (c) 2025 Arm Limited.
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
 * @file event_bridge.cpp
 *
 * @brief Implementation of SDL Wayland to X11 event forwarding bridge.
 */

#include "event_bridge.hpp"
#include "utils/logging.hpp"
#include <chrono>
#include <cstring>

extern "C" {
#include <X11/keysym.h>
#include <X11/Xutil.h>
}

namespace wsi
{
namespace x11
{

EventBridge::EventBridge(const Config& config)
   : m_x11_window(config.x11_window)
   , m_x11_display(config.x11_display)
   , m_sdl_window(config.sdl_window)
   , m_owns_x11_display(config.owns_x11_display)
   , m_active(true)
{
   if (!m_x11_window || !m_x11_display || !m_sdl_window)
   {
      WSI_LOG_ERROR("EventBridge: Invalid configuration - missing window or display");
      m_active = false;
      return;
   }

   WSI_LOG_INFO("EventBridge: Starting event forwarding (X11 window: 0x%lx, SDL window: %p)",
                m_x11_window, m_sdl_window);

   try
   {
      m_event_thread = std::thread(&EventBridge::event_forwarding_thread, this);
      WSI_LOG_INFO("EventBridge: Event forwarding thread started successfully");
   }
   catch (const std::exception& e)
   {
      WSI_LOG_ERROR("EventBridge: Failed to start event forwarding thread: %s", e.what());
      m_active = false;
   }
}

EventBridge::~EventBridge()
{
   stop();
}

void EventBridge::stop()
{
   if (m_active.exchange(false))
   {
      WSI_LOG_INFO("EventBridge: Stopping event forwarding");

      if (m_event_thread.joinable())
      {
         m_event_thread.join();
         WSI_LOG_INFO("EventBridge: Event forwarding thread stopped");
      }

      if (m_owns_x11_display && m_x11_display)
      {
         WSI_LOG_DEBUG("EventBridge: Closing X11 display");
         XCloseDisplay(m_x11_display);
         m_x11_display = nullptr;
      }
   }
}

void EventBridge::event_forwarding_thread()
{
   WSI_LOG_INFO("EventBridge: Event forwarding thread started");

   while (m_active.load())
   {
      try
      {
         sync_focus_state();

         std::this_thread::sleep_for(std::chrono::milliseconds(16));
      }
      catch (const std::exception& e)
      {
         WSI_LOG_ERROR("EventBridge: Exception in event forwarding thread: %s", e.what());
      }
   }

   WSI_LOG_INFO("EventBridge: Event forwarding thread exiting");
}

void EventBridge::sync_focus_state()
{
   Uint32 sdl_flags = SDL_GetWindowFlags(m_sdl_window);
   bool sdl_focused = (sdl_flags & SDL_WINDOW_INPUT_FOCUS) != 0;

   if (sdl_focused && !m_x11_window_focused)
   {
      WSI_LOG_DEBUG("EventBridge: SDL window focused - ensuring X11 window is focused");

      XSetInputFocus(m_x11_display, m_x11_window, RevertToParent, CurrentTime);

      XEvent focus_event;
      std::memset(&focus_event, 0, sizeof(focus_event));
      focus_event.type = FocusIn;
      focus_event.xfocus.display = m_x11_display;
      focus_event.xfocus.window = m_x11_window;
      focus_event.xfocus.mode = NotifyNormal;
      focus_event.xfocus.detail = NotifyPointer;

      send_x11_event(focus_event);
      m_x11_window_focused = true;
   }
   else if (!sdl_focused && m_x11_window_focused)
   {
      WSI_LOG_DEBUG("EventBridge: SDL window lost focus - unfocusing X11 window");

      XEvent unfocus_event;
      std::memset(&unfocus_event, 0, sizeof(unfocus_event));
      unfocus_event.type = FocusOut;
      unfocus_event.xfocus.display = m_x11_display;
      unfocus_event.xfocus.window = m_x11_window;
      unfocus_event.xfocus.mode = NotifyNormal;
      unfocus_event.xfocus.detail = NotifyPointer;

      send_x11_event(unfocus_event);
      m_x11_window_focused = false;
   }
}

bool EventBridge::send_x11_event(const XEvent& x11_event)
{
   int result = XSendEvent(m_x11_display, m_x11_window, False, 0L, /* NoEventMask */
                           const_cast<XEvent*>(&x11_event));

   if (result == 0)
   {
      WSI_LOG_WARNING("EventBridge: XSendEvent failed for event type %d", x11_event.type);
      return false;
   }

   XFlush(m_x11_display);

   return true;
}

} // namespace x11
} // namespace wsi