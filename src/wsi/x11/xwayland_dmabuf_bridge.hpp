/*
 * Copyright (c) 2026 Arm Limited.
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

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace wsi
{
namespace x11
{

class xwayland_dmabuf_bridge_client
{
public:
   static std::unique_ptr<xwayland_dmabuf_bridge_client> create_from_environment();

   explicit xwayland_dmabuf_bridge_client(std::string socket_path);
   ~xwayland_dmabuf_bridge_client();

   xwayland_dmabuf_bridge_client(const xwayland_dmabuf_bridge_client &) = delete;
   xwayland_dmabuf_bridge_client &operator=(const xwayland_dmabuf_bridge_client &) = delete;

   bool is_enabled() const;

   bool present_frame(uint32_t xid, uint32_t width, uint32_t height, uint32_t fourcc, uint64_t modifier,
                      uint32_t num_planes, const uint32_t *offsets, const int *strides, const int *plane_fds);

   void stop_stream(uint32_t xid);
   bool is_feedback_sync_enabled() const;

private:
   bool ensure_connected();
   bool probe_feedback_support();
   bool wait_for_feedback(uint32_t expected_frame_id, uint32_t timeout_ms, uint32_t &feedback_flags,
                          uint32_t &feedback_xid);
   bool send_packet(const void *packet, size_t packet_size, const int *fds, uint32_t num_fds);
   void reset_connection();

   std::string m_socket_path;
   int m_socket_fd = -1;
   bool m_connect_failed = false;
   bool m_feedback_probe_done = false;
   bool m_feedback_sync_enabled = false;
   uint32_t m_feedback_timeout_ms = 250;
   uint32_t m_next_frame_id = 1;
};

} /* namespace x11 */
} /* namespace wsi */
