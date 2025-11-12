/*
 * Copyright (c) 2021 Arm Limited.
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

/** @file
 * @brief Implementation of a x11 WSI Surface
 */

#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/shm.h>
#include <cstdlib>
#include <cstring>
#include "surface.hpp"
#include "swapchain.hpp"
#include "surface_properties.hpp"

#if BUILD_WSI_WAYLAND
extern "C" {
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
}
#include "../wayland/surface.hpp"
#include "../wayland/swapchain.hpp"
#include "sdl_wayland_swapchain_wrapper.hpp"
#include "event_bridge.hpp"
#include "utils/logging.hpp"
#endif

namespace wsi
{
namespace x11
{

struct surface::init_parameters
{
   const util::allocator &allocator;
   xcb_connection_t *connection;
   xcb_window_t window;
};

surface::surface(const init_parameters &params)
   : wsi::surface()
   , m_connection(params.connection)
   , m_window(params.window)
   , properties(this, params.allocator)
{
}

surface::~surface()
{
}

bool surface::init()
{
   auto shm_cookie = xcb_shm_query_version_unchecked(m_connection);
   auto shm_reply = xcb_shm_query_version_reply(m_connection, shm_cookie, nullptr);

   m_has_shm = shm_reply != nullptr;
   free(shm_reply);
   return true;
}

bool surface::get_size_and_depth(uint32_t *width, uint32_t *height, int *depth)
{
   auto cookie = xcb_get_geometry(m_connection, m_window);
   if (auto *geom = xcb_get_geometry_reply(m_connection, cookie, nullptr))
   {
      *width = static_cast<uint32_t>(geom->width);
      *height = static_cast<uint32_t>(geom->height);
      *depth = static_cast<int>(geom->depth);
      free(geom);
      return true;
   }
   return false;
}

wsi::surface_properties &surface::get_properties()
{
   return properties;
}

util::unique_ptr<swapchain_base> surface::allocate_swapchain(wsi::device_private_data &dev_data,
                                                             const VkAllocationCallbacks *allocator)
{
   util::allocator alloc{ dev_data.get_allocator(), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, allocator };

#if BUILD_WSI_WAYLAND
   if (should_use_wayland_via_sdl())
   {
      WSI_LOG_INFO("=== SDL WAYLAND ROUTING ACTIVATED ===");
      WSI_LOG_INFO("SDL Wayland driver detected - routing to Wayland swapchain");
      return create_wayland_swapchain_via_sdl(dev_data, allocator, alloc);
   }
   else
   {
      WSI_LOG_INFO("=== USING X11 SWAPCHAIN PATH ===");
   }
#endif

   auto sc = alloc.make_unique<swapchain>(dev_data, allocator, *this);


   return util::unique_ptr<swapchain_base>(std::move(sc));
}

util::unique_ptr<surface> surface::make_surface(const util::allocator &allocator, xcb_connection_t *conn,
                                                xcb_window_t window)
{
   xcb_get_geometry_cookie_t test_cookie = xcb_get_geometry(conn, window);
   xcb_generic_error_t *test_error = nullptr;
   xcb_get_geometry_reply_t *test_geom = xcb_get_geometry_reply(conn, test_cookie, &test_error);
   if (test_error)
   {
      free(test_error);
   }
   else if (test_geom)
   {
      free(test_geom);
   }
   else
   {
      WSI_LOG_WARNING("Window 0x%x query returned NULL during surface creation\n", window);
   }

   init_parameters params{ allocator, conn, window };
   auto wsi_surface = allocator.make_unique<surface>(params);
   if (wsi_surface != nullptr)
   {
      if (wsi_surface->init())
      {
         return wsi_surface;
      }
      else
      {
         WSI_LOG_ERROR("Surface init failed for window 0x%x\n", window);
      }
   }
   else
   {
      WSI_LOG_ERROR("Failed to allocate surface for window 0x%x\n", window);
   }
   return nullptr;
}

#if BUILD_WSI_WAYLAND
bool surface::should_use_wayland_via_sdl() const
{
   const char* force_sdl_wayland = std::getenv("WSI_FORCE_SDL_WAYLAND");
   if (force_sdl_wayland && strcmp(force_sdl_wayland, "1") == 0)
   {
      WSI_LOG_INFO("WSI_FORCE_SDL_WAYLAND=1 detected - forcing SDL Wayland routing");
      return true;
   }

   if (SDL_WasInit(SDL_INIT_VIDEO) != 0)
   {
      const char* current_driver = SDL_GetCurrentVideoDriver();
      if (current_driver)
      {
         bool is_wayland = (strcmp(current_driver, "wayland") == 0);
         if (is_wayland)
         {
            WSI_LOG_INFO("SDL already initialized with Wayland driver: %s", current_driver);
            return true;
         }
      }
   }

   bool was_video_init = (SDL_WasInit(SDL_INIT_VIDEO) != 0);

   if (!was_video_init)
   {
      if (SDL_WasInit(SDL_INIT_TIMER) == 0)
      {
         if (SDL_InitSubSystem(SDL_INIT_TIMER) != 0)
         {
            WSI_LOG_DEBUG("Failed to initialize SDL timer subsystem: %s", SDL_GetError());
            return false;
         }
      }

      if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
      {
         WSI_LOG_DEBUG("Failed to initialize SDL video subsystem: %s", SDL_GetError());
         return false;
      }
   }

   const char* detected_driver = SDL_GetCurrentVideoDriver();
   bool is_wayland = false;

   if (detected_driver)
   {
      is_wayland = (strcmp(detected_driver, "wayland") == 0);
      WSI_LOG_INFO("SDL video driver detected: %s (Wayland: %s)",
                   detected_driver, is_wayland ? "yes" : "no");
   }
   else
   {
      WSI_LOG_WARNING("Could not detect SDL video driver");
   }

   if (!was_video_init)
   {
      SDL_QuitSubSystem(SDL_INIT_VIDEO);
   }

   return is_wayland;
}

util::unique_ptr<swapchain_base> surface::create_wayland_swapchain_via_sdl(wsi::device_private_data &dev_data,
                                                                           const VkAllocationCallbacks *allocator,
                                                                           util::allocator &alloc)
{
   WSI_LOG_INFO("Creating Wayland swapchain via SDL surface extraction");

   try
   {
      SDL_SetHint(SDL_HINT_VIDEODRIVER, "wayland");

      if (SDL_Init(SDL_INIT_VIDEO) != 0)
      {
         WSI_LOG_ERROR("Failed to initialize SDL video subsystem: %s", SDL_GetError());
         return nullptr;
      }

      SDL_DisplayMode display_mode;
      int display_index = 0;
      if (SDL_GetDesktopDisplayMode(display_index, &display_mode) != 0)
      {
         WSI_LOG_ERROR("Failed to get desktop display mode: %s", SDL_GetError());
         SDL_Quit();
         return nullptr;
      }

      int width = display_mode.w;
      int height = display_mode.h;
      WSI_LOG_INFO("Option 1 - SDL display resolution: %dx%d", width, height);

      bool is_x11_fullscreen = false;

      auto geom_cookie = xcb_get_geometry(m_connection, m_window);
      auto geom_reply = xcb_get_geometry_reply(m_connection, geom_cookie, nullptr);

      bool size_matches_display = false;
      if (geom_reply)
      {
         size_matches_display = (geom_reply->width == width && geom_reply->height == height);
         WSI_LOG_INFO("X11 window geometry: %dx%d (display: %dx%d) - size match: %s",
                     geom_reply->width, geom_reply->height, width, height,
                     size_matches_display ? "yes" : "no");
         free(geom_reply);
      }

      xcb_intern_atom_cookie_t state_cookie = xcb_intern_atom(m_connection, 0, 13, "_NET_WM_STATE");
      xcb_intern_atom_reply_t* state_reply = xcb_intern_atom_reply(m_connection, state_cookie, nullptr);

      if (state_reply)
      {
         xcb_atom_t state_atom = state_reply->atom;
         free(state_reply);

         xcb_intern_atom_cookie_t fullscreen_cookie = xcb_intern_atom(m_connection, 0, 24, "_NET_WM_STATE_FULLSCREEN");
         xcb_intern_atom_reply_t* fullscreen_reply = xcb_intern_atom_reply(m_connection, fullscreen_cookie, nullptr);

         if (fullscreen_reply)
         {
            xcb_atom_t fullscreen_atom = fullscreen_reply->atom;
            free(fullscreen_reply);

            xcb_get_property_cookie_t prop_cookie = xcb_get_property(
               m_connection, 0, m_window,
               state_atom, XCB_ATOM_ATOM, 0, 1024
            );
            xcb_get_property_reply_t* prop_reply = xcb_get_property_reply(m_connection, prop_cookie, nullptr);

            if (prop_reply)
            {
               xcb_atom_t* atoms = static_cast<xcb_atom_t*>(xcb_get_property_value(prop_reply));
               int num_atoms = xcb_get_property_value_length(prop_reply) / sizeof(xcb_atom_t);

               WSI_LOG_INFO("X11 window has %d _NET_WM_STATE atoms", num_atoms);

               for (int i = 0; i < num_atoms; i++)
               {
                  if (atoms[i] == fullscreen_atom)
                  {
                     is_x11_fullscreen = true;
                     WSI_LOG_INFO("Found _NET_WM_STATE_FULLSCREEN atom");
                     break;
                  }
               }
               free(prop_reply);
            }
            else
            {
               WSI_LOG_INFO("No _NET_WM_STATE property found on window");
            }
         }
      }

      bool treat_as_fullscreen = is_x11_fullscreen || size_matches_display;

      if (!treat_as_fullscreen)
      {
         WSI_LOG_WARNING("X11 window is not in fullscreen mode - falling back to X11 swapchain");

         if (m_sdl_wayland_surface)
         {
            WSI_LOG_INFO("Destroying existing SDL Wayland surface before fallback");
            m_sdl_wayland_surface.reset();
         }
         if (m_event_bridge)
         {
            m_event_bridge.reset();
         }

         SDL_Quit();
         WSI_LOG_INFO("SDL resources cleaned up before X11 fallback");

         xcb_intern_atom_cookie_t opacity_cookie = xcb_intern_atom(m_connection, 0, 22, "_NET_WM_WINDOW_OPACITY");
         xcb_intern_atom_reply_t* opacity_reply = xcb_intern_atom_reply(m_connection, opacity_cookie, nullptr);

         if (opacity_reply)
         {
            xcb_atom_t opacity_atom = opacity_reply->atom;
            free(opacity_reply);

            uint32_t opacity_value = 0xFFFFFFFF; // Fully opaque

            xcb_change_property(
               m_connection,
               XCB_PROP_MODE_REPLACE,
               m_window,
               opacity_atom,
               XCB_ATOM_CARDINAL,
               32,
               1,
               &opacity_value
            );
            xcb_flush(m_connection);
            WSI_LOG_INFO("Restored X11 window opacity to fully opaque for fallback");
         }
         else
         {
            WSI_LOG_WARNING("Failed to get opacity atom - X11 window may remain transparent");
         }

         auto sc = alloc.make_unique<swapchain>(dev_data, allocator, *this);
         return util::unique_ptr<swapchain_base>(std::move(sc));
      }

      WSI_LOG_INFO("X11 window is fullscreen - proceeding with SDL window creation");

      SDL_Window* sdl_window = SDL_CreateWindow(
         "Mali WSI SDL Wayland Surface",
         SDL_WINDOWPOS_UNDEFINED,
         SDL_WINDOWPOS_UNDEFINED,
         width, height,
         SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP
      );

      if (!sdl_window)
      {
         WSI_LOG_ERROR("Failed to create SDL window: %s", SDL_GetError());
         SDL_Quit();
         return nullptr;
      }

      SDL_SysWMinfo wm_info;
      SDL_VERSION(&wm_info.version);

      if (!SDL_GetWindowWMInfo(sdl_window, &wm_info) || wm_info.subsystem != SDL_SYSWM_WAYLAND)
      {
         WSI_LOG_ERROR("Failed to get Wayland handles from SDL");
         SDL_DestroyWindow(sdl_window);
         SDL_Quit();
         return nullptr;
      }

      struct wl_display* wayland_display = wm_info.info.wl.display;
      struct wl_surface* wayland_surface = wm_info.info.wl.surface;

      if (!wayland_display || !wayland_surface)
      {
         WSI_LOG_ERROR("Invalid Wayland handles from SDL");
         SDL_DestroyWindow(sdl_window);
         SDL_Quit();
         return nullptr;
      }

      WSI_LOG_INFO("Extracted Wayland handles: display=%p, surface=%p", wayland_display, wayland_surface);

      xcb_clear_area(m_connection, 1, m_window, 0, 0, 0, 0);
      xcb_flush(m_connection);
      WSI_LOG_INFO("Cleared X11 window contents to prevent stale frame display");

      xcb_intern_atom_cookie_t opacity_cookie = xcb_intern_atom(m_connection, 0, 22, "_NET_WM_WINDOW_OPACITY");
      xcb_intern_atom_reply_t* opacity_reply = xcb_intern_atom_reply(m_connection, opacity_cookie, nullptr);

      if (opacity_reply)
      {
         xcb_atom_t opacity_atom = opacity_reply->atom;
         free(opacity_reply);

         uint32_t opacity_value = 0;

         xcb_change_property(
            m_connection,
            XCB_PROP_MODE_REPLACE,
            m_window,
            opacity_atom,
            XCB_ATOM_CARDINAL,
            32,
            1,
            &opacity_value
         );
         xcb_flush(m_connection);
         WSI_LOG_INFO("Set X11 window opacity to 0 (fully transparent)");

         xcb_get_property_cookie_t prop_cookie = xcb_get_property(
            m_connection, 0, m_window, opacity_atom, XCB_ATOM_CARDINAL, 0, 1
         );
         xcb_get_property_reply_t* prop_reply = xcb_get_property_reply(m_connection, prop_cookie, nullptr);
         if (prop_reply)
         {
            uint32_t* opacity_val = static_cast<uint32_t*>(xcb_get_property_value(prop_reply));
            if (opacity_val)
            {
               WSI_LOG_INFO("Verified opacity value: 0x%08x (0=transparent, 0xFFFFFFFF=opaque)", *opacity_val);
            }
            free(prop_reply);
         }
      }
      else
      {
         WSI_LOG_WARNING("Failed to get _NET_WM_WINDOW_OPACITY atom - X11 window may remain visible");
      }

      m_sdl_wayland_surface = wayland::surface::make_surface_external(
         alloc,
         wayland_display,
         wayland_surface,
         sdl_window
      );

      if (!m_sdl_wayland_surface)
      {
         WSI_LOG_ERROR("Failed to create Wayland surface");
         SDL_DestroyWindow(sdl_window);
         SDL_Quit();
         return nullptr;
      }

      Display* x11_display = nullptr;
      try
      {
         x11_display = XOpenDisplay(nullptr);
         if (!x11_display)
         {
            WSI_LOG_WARNING("Failed to open X11 display for event forwarding - events may not work properly");
         }
         else
         {
            Window x11_window = static_cast<Window>(m_window);

            wsi::x11::EventBridge::Config bridge_config;
            bridge_config.x11_window = x11_window;
            bridge_config.x11_display = x11_display;
            bridge_config.sdl_window = sdl_window;
            bridge_config.owns_x11_display = true;

            m_event_bridge = std::make_unique<wsi::x11::EventBridge>(bridge_config);

            if (m_event_bridge->is_active())
            {
               WSI_LOG_INFO("EventBridge created successfully - SDL events will be forwarded to X11 window");
            }
            else
            {
               WSI_LOG_WARNING("EventBridge creation failed - controller/input events may not work properly");
               m_event_bridge.reset();
               XCloseDisplay(x11_display);
               x11_display = nullptr;
            }
         }
      }
      catch (const std::exception& e)
      {
         WSI_LOG_WARNING("Exception creating EventBridge: %s - events may not work properly", e.what());
         if (x11_display)
         {
            XCloseDisplay(x11_display);
            x11_display = nullptr;
         }
      }

      auto sdl_wayland_wrapper = alloc.make_unique<sdl_wayland_swapchain_wrapper>(dev_data, allocator, *m_sdl_wayland_surface);

      if (!sdl_wayland_wrapper)
      {
         WSI_LOG_ERROR("Failed to create SDL Wayland swapchain wrapper");
         SDL_Quit();
         return nullptr;
      }

      WSI_LOG_INFO("Successfully created multi-threaded SDL Wayland swapchain wrapper");
      return util::unique_ptr<swapchain_base>(std::move(sdl_wayland_wrapper));
   }
   catch (const std::exception &e)
   {
      WSI_LOG_ERROR("Exception creating Wayland swapchain via SDL: %s", e.what());
      SDL_Quit();
      return nullptr;
   }
}
#endif

} /* namespace x11 */
} /* namespace wsi */
