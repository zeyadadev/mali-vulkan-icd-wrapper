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

#include "xwayland_dmabuf_bridge.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "utils/logging.hpp"

namespace wsi
{
namespace x11
{
namespace
{
constexpr uint32_t XWL_DMABUF_BRIDGE_MAGIC = 0x58444246u;
constexpr uint16_t XWL_DMABUF_BRIDGE_VERSION = 1;
constexpr uint16_t XWL_DMABUF_BRIDGE_OP_FRAME = 1;
constexpr uint16_t XWL_DMABUF_BRIDGE_OP_STOP = 2;
constexpr uint16_t XWL_DMABUF_BRIDGE_OP_HELLO = 3;
constexpr uint16_t XWL_DMABUF_BRIDGE_OP_FEEDBACK = 4;

constexpr uint32_t XWL_DMABUF_BRIDGE_FEEDBACK_FAILED = 1u << 0;
constexpr uint32_t XWL_DMABUF_BRIDGE_FEEDBACK_CAP_SYNC = 1u << 16;

constexpr uint32_t XWL_DMABUF_BRIDGE_HELLO_FRAME_ID = 0x48454c4fu; /* "HELO" */
constexpr uint32_t XWL_DMABUF_BRIDGE_MAX_PLANES = 4;

struct xwl_dmabuf_bridge_plane
{
   uint32_t offset;
   uint32_t stride;
   uint32_t modifier_hi;
   uint32_t modifier_lo;
};

struct xwl_dmabuf_bridge_packet
{
   uint32_t magic;
   uint16_t version;
   uint16_t opcode;
   uint32_t xid;
   uint32_t width;
   uint32_t height;
   uint32_t format;
   uint32_t flags;
   uint32_t num_planes;
   uint32_t reserved;
   xwl_dmabuf_bridge_plane planes[XWL_DMABUF_BRIDGE_MAX_PLANES];
};
} /* namespace */

std::unique_ptr<xwayland_dmabuf_bridge_client> xwayland_dmabuf_bridge_client::create_from_environment()
{
   const char *socket_path = std::getenv("XWL_DMABUF_BRIDGE");
   if (!socket_path || !socket_path[0])
   {
      return nullptr;
   }

   return std::make_unique<xwayland_dmabuf_bridge_client>(std::string(socket_path));
}

xwayland_dmabuf_bridge_client::xwayland_dmabuf_bridge_client(std::string socket_path)
   : m_socket_path(std::move(socket_path))
{
   const char *timeout_env = std::getenv("XWL_DMABUF_BRIDGE_FEEDBACK_TIMEOUT_MS");
   if (timeout_env && timeout_env[0] != '\0')
   {
      errno = 0;
      char *end = nullptr;
      unsigned long value = std::strtoul(timeout_env, &end, 10);
      if (errno == 0 && end != timeout_env && *end == '\0')
      {
         m_feedback_timeout_ms = static_cast<uint32_t>(value > 5000 ? 5000 : value);
      }
      else
      {
         WSI_LOG_WARNING("Xwayland bridge: invalid XWL_DMABUF_BRIDGE_FEEDBACK_TIMEOUT_MS='%s', using default %u ms",
                         timeout_env, m_feedback_timeout_ms);
      }
   }
}

xwayland_dmabuf_bridge_client::~xwayland_dmabuf_bridge_client()
{
   reset_connection();
}

bool xwayland_dmabuf_bridge_client::is_enabled() const
{
   return !m_socket_path.empty();
}

bool xwayland_dmabuf_bridge_client::is_feedback_sync_enabled() const
{
   return m_feedback_sync_enabled;
}

bool xwayland_dmabuf_bridge_client::present_frame(uint32_t xid, uint32_t width, uint32_t height, uint32_t fourcc,
                                                  uint64_t modifier, uint32_t num_planes, const uint32_t *offsets,
                                                  const int *strides, const int *plane_fds)
{
   if (!is_enabled() || num_planes == 0 || num_planes > XWL_DMABUF_BRIDGE_MAX_PLANES)
   {
      return false;
   }

   xwl_dmabuf_bridge_packet packet = {};
   packet.magic = XWL_DMABUF_BRIDGE_MAGIC;
   packet.version = XWL_DMABUF_BRIDGE_VERSION;
   packet.opcode = XWL_DMABUF_BRIDGE_OP_FRAME;
   packet.xid = xid;
   packet.width = width;
   packet.height = height;
   packet.format = fourcc;
   packet.flags = 0;
   packet.num_planes = num_planes;
   packet.reserved = m_next_frame_id++;
   if (packet.reserved == 0)
   {
      packet.reserved = m_next_frame_id++;
   }

   const uint32_t modifier_hi = static_cast<uint32_t>(modifier >> 32);
   const uint32_t modifier_lo = static_cast<uint32_t>(modifier & 0xffffffffu);

   for (uint32_t i = 0; i < num_planes; i++)
   {
      if (plane_fds[i] < 0 || strides[i] < 0)
      {
         return false;
      }

      packet.planes[i].offset = offsets[i];
      packet.planes[i].stride = static_cast<uint32_t>(strides[i]);
      packet.planes[i].modifier_hi = modifier_hi;
      packet.planes[i].modifier_lo = modifier_lo;
   }

   if (!send_packet(&packet, sizeof(packet), plane_fds, num_planes))
   {
      return false;
   }

   if (!m_feedback_sync_enabled)
   {
      return true;
   }

   uint32_t feedback_flags = 0;
   uint32_t feedback_xid = 0;
   if (!wait_for_feedback(packet.reserved, m_feedback_timeout_ms, feedback_flags, feedback_xid))
   {
      WSI_LOG_WARNING("Xwayland bridge: timed out waiting for feedback (frame=%u, xid=0x%x), disabling sync feedback",
                      packet.reserved, xid);
      m_feedback_sync_enabled = false;
      return true;
   }

   if (feedback_flags & XWL_DMABUF_BRIDGE_FEEDBACK_FAILED)
   {
      WSI_LOG_WARNING("Xwayland bridge: compositor rejected frame via feedback (frame=%u, xid=0x%x ack_xid=0x%x)",
                      packet.reserved, xid, feedback_xid);
      return true;
   }

   return true;
}

void xwayland_dmabuf_bridge_client::stop_stream(uint32_t xid)
{
   if (!is_enabled())
   {
      return;
   }

   xwl_dmabuf_bridge_packet packet = {};
   packet.magic = XWL_DMABUF_BRIDGE_MAGIC;
   packet.version = XWL_DMABUF_BRIDGE_VERSION;
   packet.opcode = XWL_DMABUF_BRIDGE_OP_STOP;
   packet.xid = xid;

   send_packet(&packet, sizeof(packet), nullptr, 0);
}

bool xwayland_dmabuf_bridge_client::ensure_connected()
{
   if (!is_enabled())
   {
      return false;
   }

   if (m_socket_fd >= 0)
   {
      return true;
   }

   if (m_connect_failed)
   {
      return false;
   }

   int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
   if (fd < 0)
   {
      WSI_LOG_WARNING("Xwayland bridge socket() failed: %s", strerror(errno));
      m_connect_failed = true;
      return false;
   }

   int flags = fcntl(fd, F_GETFD);
   if (flags >= 0)
   {
      (void) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
   }

   sockaddr_un addr = {};
   addr.sun_family = AF_UNIX;
   if (m_socket_path.size() >= sizeof(addr.sun_path))
   {
      WSI_LOG_WARNING("Xwayland bridge path is too long: %s", m_socket_path.c_str());
      close(fd);
      m_connect_failed = true;
      return false;
   }
   std::strncpy(addr.sun_path, m_socket_path.c_str(), sizeof(addr.sun_path) - 1);

   if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
   {
      WSI_LOG_WARNING("Xwayland bridge connect(%s) failed: %s", m_socket_path.c_str(), strerror(errno));
      close(fd);
      m_connect_failed = true;
      return false;
   }

   m_socket_fd = fd;
   m_connect_failed = false;
   m_feedback_probe_done = false;
   m_feedback_sync_enabled = false;
   WSI_LOG_INFO("Connected to Xwayland dmabuf bridge at %s", m_socket_path.c_str());
   (void) probe_feedback_support();
   return true;
}

bool xwayland_dmabuf_bridge_client::probe_feedback_support()
{
   if (m_feedback_probe_done)
   {
      return m_feedback_sync_enabled;
   }

   m_feedback_probe_done = true;

   xwl_dmabuf_bridge_packet packet = {};
   packet.magic = XWL_DMABUF_BRIDGE_MAGIC;
   packet.version = XWL_DMABUF_BRIDGE_VERSION;
   packet.opcode = XWL_DMABUF_BRIDGE_OP_HELLO;
   packet.reserved = XWL_DMABUF_BRIDGE_HELLO_FRAME_ID;

   if (!send_packet(&packet, sizeof(packet), nullptr, 0))
   {
      return false;
   }

   uint32_t feedback_flags = 0;
   uint32_t feedback_xid = 0;
   if (!wait_for_feedback(XWL_DMABUF_BRIDGE_HELLO_FRAME_ID, 100, feedback_flags, feedback_xid))
   {
      WSI_LOG_INFO("Xwayland bridge: sync feedback unsupported by server, using fallback pacing");
      return false;
   }

   if (feedback_flags & XWL_DMABUF_BRIDGE_FEEDBACK_CAP_SYNC)
   {
      m_feedback_sync_enabled = true;
      WSI_LOG_INFO("Xwayland bridge: sync feedback enabled (ack-based pacing)");
      return true;
   }

   WSI_LOG_INFO("Xwayland bridge: sync feedback unsupported by server, using fallback pacing");
   return false;
}

bool xwayland_dmabuf_bridge_client::wait_for_feedback(uint32_t expected_frame_id, uint32_t timeout_ms,
                                                       uint32_t &feedback_flags, uint32_t &feedback_xid)
{
   if (m_socket_fd < 0)
   {
      return false;
   }

   const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

   while (true)
   {
      int remaining_ms = 0;
      if (timeout_ms == 0)
      {
         remaining_ms = 0;
      }
      else
      {
         const auto now = std::chrono::steady_clock::now();
         if (now >= deadline)
         {
            return false;
         }
         remaining_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
         if (remaining_ms <= 0)
         {
            remaining_ms = 1;
         }
      }

      pollfd pfd = {};
      pfd.fd = m_socket_fd;
      pfd.events = POLLIN;

      int poll_res = poll(&pfd, 1, remaining_ms);
      if (poll_res < 0)
      {
         if (errno == EINTR)
         {
            continue;
         }
         WSI_LOG_WARNING("Xwayland bridge poll() failed while waiting feedback: %s", strerror(errno));
         reset_connection();
         return false;
      }

      if (poll_res == 0)
      {
         return false;
      }

      if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
      {
         WSI_LOG_WARNING("Xwayland bridge feedback channel closed (revents=0x%x)", pfd.revents);
         reset_connection();
         return false;
      }

      if (!(pfd.revents & POLLIN))
      {
         continue;
      }

      xwl_dmabuf_bridge_packet packet = {};
      ssize_t received = recv(m_socket_fd, &packet, sizeof(packet), MSG_DONTWAIT);
      if (received < 0)
      {
         if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
         {
            continue;
         }
         WSI_LOG_WARNING("Xwayland bridge recv() failed while waiting feedback: %s", strerror(errno));
         reset_connection();
         return false;
      }

      if (received == 0)
      {
         reset_connection();
         return false;
      }

      if (static_cast<size_t>(received) != sizeof(packet))
      {
         WSI_LOG_WARNING("Xwayland bridge feedback packet size mismatch: expected=%zu got=%zd", sizeof(packet),
                         received);
         continue;
      }

      if (packet.magic != XWL_DMABUF_BRIDGE_MAGIC || packet.version != XWL_DMABUF_BRIDGE_VERSION)
      {
         continue;
      }

      if (packet.opcode != XWL_DMABUF_BRIDGE_OP_FEEDBACK)
      {
         continue;
      }

      if (packet.reserved != expected_frame_id)
      {
         continue;
      }

      feedback_flags = packet.flags;
      feedback_xid = packet.xid;
      return true;
   }
}

bool xwayland_dmabuf_bridge_client::send_packet(const void *packet, size_t packet_size, const int *fds, uint32_t num_fds)
{
   if (!ensure_connected())
   {
      return false;
   }

   iovec iov = {};
   iov.iov_base = const_cast<void *>(packet);
   iov.iov_len = packet_size;

   msghdr msg = {};
   msg.msg_iov = &iov;
   msg.msg_iovlen = 1;

   char control[CMSG_SPACE(sizeof(int) * XWL_DMABUF_BRIDGE_MAX_PLANES)] = {};
   if (fds && num_fds > 0)
   {
      if (num_fds > XWL_DMABUF_BRIDGE_MAX_PLANES)
      {
         return false;
      }

      msg.msg_control = control;
      msg.msg_controllen = CMSG_SPACE(sizeof(int) * num_fds);

      cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
      cmsg->cmsg_level = SOL_SOCKET;
      cmsg->cmsg_type = SCM_RIGHTS;
      cmsg->cmsg_len = CMSG_LEN(sizeof(int) * num_fds);
      std::memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * num_fds);
   }

   ssize_t sent = sendmsg(m_socket_fd, &msg, MSG_NOSIGNAL);
   if (sent < 0)
   {
      WSI_LOG_WARNING("Xwayland bridge sendmsg failed: %s", strerror(errno));
      reset_connection();
      return false;
   }

   if (static_cast<size_t>(sent) != packet_size)
   {
      WSI_LOG_WARNING("Xwayland bridge short send: expected=%zu sent=%zd", packet_size, sent);
      reset_connection();
      return false;
   }

   return true;
}

void xwayland_dmabuf_bridge_client::reset_connection()
{
   if (m_socket_fd >= 0)
   {
      close(m_socket_fd);
      m_socket_fd = -1;
   }
   m_feedback_probe_done = false;
   m_feedback_sync_enabled = false;
}

} /* namespace x11 */
} /* namespace wsi */
