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
 * @file event_bridge.hpp
 *
 * @brief Event forwarding bridge between SDL Wayland and X11 windows.
 *
 * This component solves the performance vs compatibility problem:
 * - Apps create X11 windows and expect X11 events (controller, mouse, keyboard)
 * - We route to SDL Wayland for zero-copy dmabuf performance
 * - EventBridge forwards SDL events to the original X11 window
 * - Result: Zero-copy performance + full event compatibility
 */

#pragma once

#include <atomic>
#include <thread>
#include <memory>

extern "C" {
#include <X11/Xlib.h>
#include <SDL2/SDL.h>
}

namespace wsi
{
namespace x11
{

/**
 * @brief Event forwarding bridge between SDL Wayland window and X11 window
 *
 * This class creates a background thread that:
 * 1. Polls SDL events from the Wayland window (where user actually interacts)
 * 2. Translates them to X11 events
 * 3. Forwards them to the original X11 window (where app expects them)
 * 4. Keeps the X11 window focused so controller/input works
 *
 * Critical for apps like:
 * - Dolphin Emulator: Needs focus events for controller input
 * - Wine applications: Needs mouse motion events
 */
class EventBridge
{
public:
   /**
    * @brief Configuration for event bridge creation
    */
   struct Config
   {
      Window x11_window = 0;           ///< Original X11 window handle
      Display* x11_display = nullptr;  ///< X11 display connection
      SDL_Window* sdl_window = nullptr; ///< SDL Wayland window
      bool owns_x11_display = false;   ///< Whether EventBridge should close display in destructor
   };

   /**
    * @brief Create and start event forwarding bridge
    *
    * @param config Bridge configuration
    */
   explicit EventBridge(const Config& config);

   /**
    * @brief Stop event forwarding and cleanup
    */
   ~EventBridge();

   // Non-copyable, non-movable
   EventBridge(const EventBridge&) = delete;
   EventBridge& operator=(const EventBridge&) = delete;
   EventBridge(EventBridge&&) = delete;
   EventBridge& operator=(EventBridge&&) = delete;

   /**
    * @brief Check if event forwarding is active
    *
    * @return true if thread is running and forwarding events
    */
   bool is_active() const { return m_active.load(); }

   /**
    * @brief Stop event forwarding (called automatically by destructor)
    */
   void stop();

private:
   /**
    * @brief Background thread function that forwards events
    */
   void event_forwarding_thread();

   /**
    * @brief Synchronize focus state between SDL and X11 windows
    */
   void sync_focus_state();

   /**
    * @brief Send X11 event to the target window
    *
    * @param x11_event Event to send
    * @return true if event was sent successfully
    */
   bool send_x11_event(const XEvent& x11_event);

private:
   Window m_x11_window;
   Display* m_x11_display;
   SDL_Window* m_sdl_window;
   bool m_owns_x11_display = false;

   std::atomic<bool> m_active;
   std::thread m_event_thread;

   bool m_x11_window_focused = false;
   bool m_sdl_window_focused = false;
};

} // namespace x11
} // namespace wsi