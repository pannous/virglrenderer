/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "proxy_socket.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

/* macOS compatibility - these flags don't exist on macOS */
#ifdef __APPLE__
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/*
 * macOS doesn't support SOCK_SEQPACKET for Unix domain sockets.
 * Use SOCK_STREAM with message framing instead.
 */
struct stream_msg_header {
   uint32_t size;      /* payload size */
   uint32_t fd_count;  /* number of fds attached */
};

/* Read exactly n bytes from a stream socket */
static bool
proxy_socket_read_all(int fd, void *buf, size_t size)
{
   char *p = buf;
   size_t remaining = size;
   while (remaining > 0) {
      ssize_t n = read(fd, p, remaining);
      if (n < 0) {
         if (errno == EINTR || errno == EAGAIN)
            continue;
         proxy_log("failed to read from socket: %s", strerror(errno));
         return false;
      }
      if (n == 0) {
         /* EOF */
         return false;
      }
      p += n;
      remaining -= n;
   }
   return true;
}

/* Write exactly n bytes to a stream socket */
static bool
proxy_socket_write_all(int fd, const void *buf, size_t size)
{
   const char *p = buf;
   size_t remaining = size;
   while (remaining > 0) {
      ssize_t n = write(fd, p, remaining);
      if (n < 0) {
         if (errno == EINTR || errno == EAGAIN)
            continue;
         proxy_log("failed to write to socket: %s", strerror(errno));
         return false;
      }
      p += n;
      remaining -= n;
   }
   return true;
}

/* Set close-on-exec flag on fd */
static void
set_cloexec(int fd)
{
   int flags = fcntl(fd, F_GETFD);
   if (flags >= 0)
      fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}
#endif /* __APPLE__ */

#define PROXY_SOCKET_MAX_FD_COUNT 8

/* this is only used when the render server is started on demand */
bool
proxy_socket_pair(int out_fds[static 2])
{
#ifdef __APPLE__
   /* macOS doesn't support SOCK_SEQPACKET, use SOCK_STREAM
    * Note: Don't set CLOEXEC here because these fds are used across fork+exec
    * to the render_server child process. The child needs to inherit fd[1].
    */
   int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, out_fds);
   if (ret) {
      proxy_log("failed to create socket pair");
      return false;
   }
   /* Only set cloexec on the fd that stays in the parent process (fd[0]).
    * fd[1] is passed to the child process via exec, so it must NOT have cloexec.
    */
   set_cloexec(out_fds[0]);
#else
   int ret = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, out_fds);
   if (ret) {
      proxy_log("failed to create socket pair");
      return false;
   }
#endif

   return true;
}

bool
proxy_socket_is_seqpacket(int fd)
{
   int type;
   socklen_t len = sizeof(type);
   if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len)) {
      proxy_log("fd %d err %s", fd, strerror(errno));
      return false;
   }
#ifdef __APPLE__
   /* On macOS we use SOCK_STREAM with message framing */
   return type == SOCK_STREAM || type == SOCK_SEQPACKET;
#else
   return type == SOCK_SEQPACKET;
#endif
}

void
proxy_socket_init(struct proxy_socket *socket, int fd)
{
   /* TODO make fd non-blocking and perform io with timeout */
   assert(fd >= 0);
   *socket = (struct proxy_socket){
      .fd = fd,
   };
}

void
proxy_socket_fini(struct proxy_socket *socket)
{
   close(socket->fd);
}

bool
proxy_socket_is_connected(const struct proxy_socket *socket)
{
   struct pollfd poll_fd = {
      .fd = socket->fd,
   };

   while (true) {
      const int ret = poll(&poll_fd, 1, 0);
      if (ret == 0) {
         return true;
      } else if (ret < 0) {
         if (errno == EINTR || errno == EAGAIN)
            continue;

         proxy_log("failed to poll socket");
         return false;
      }

      if (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
         proxy_log("socket disconnected");
         return false;
      }

      return true;
   }
}

static const int *
get_received_fds(const struct msghdr *msg, int *out_count)
{
   const struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);
   if (unlikely(!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
                cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len < CMSG_LEN(0))) {
      *out_count = 0;
      return NULL;
   }

   *out_count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
   return (const int *)CMSG_DATA(cmsg);
}

static bool
proxy_socket_recvmsg(struct proxy_socket *socket, struct msghdr *msg, size_t *out_size)
{
   do {
      const ssize_t s = recvmsg(socket->fd, msg, MSG_CMSG_CLOEXEC);
      if (unlikely(s < 0)) {
         if (errno == EAGAIN || errno == EINTR)
            continue;

         proxy_log("failed to receive message: %s", strerror(errno));
         return false;
      }

      if (unlikely(s == 0)) {
         /* EOF / connection closed */
         return false;
      }

      assert(msg->msg_iovlen == 1);
      if (unlikely(msg->msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
         proxy_log("failed to receive message: truncated");

         int fd_count;
         const int *fds = get_received_fds(msg, &fd_count);
         for (int i = 0; i < fd_count; i++)
            close(fds[i]);

         return false;
      }

#ifndef __APPLE__
      /* SOCK_SEQPACKET: expect exact message size */
      if (unlikely(msg->msg_iov[0].iov_len != (size_t)s)) {
         proxy_log("failed to receive message: incomplete");

         int fd_count;
         const int *fds = get_received_fds(msg, &fd_count);
         for (int i = 0; i < fd_count; i++)
            close(fds[i]);

         return false;
      }
#else
      /* macOS doesn't support MSG_CMSG_CLOEXEC, set CLOEXEC manually */
      {
         int fd_count;
         const int *fds = get_received_fds(msg, &fd_count);
         for (int i = 0; i < fd_count; i++)
            set_cloexec(fds[i]);
      }
#endif

      if (out_size)
         *out_size = s;
      return true;
   } while (true);
}

static bool
proxy_socket_receive_reply_internal(struct proxy_socket *socket,
                                    void *data,
                                    size_t size,
                                    int *fds,
                                    int max_fd_count,
                                    int *out_fd_count)
{
   assert(data && size);

#ifdef __APPLE__
   /* On macOS with SOCK_STREAM, we use message framing:
    * 1. Read 8-byte header (size + fd_count)
    * 2. Receive exactly that many bytes with recvmsg for fds
    */
   struct stream_msg_header hdr;
   proxy_log("proxy_receive_reply: reading framing header, expecting size=%zu", size);
   if (!proxy_socket_read_all(socket->fd, &hdr, sizeof(hdr)))
      return false;

   proxy_log("proxy_receive_reply: got header size=%u fd_count=%u", hdr.size, hdr.fd_count);
   if (hdr.size != size) {
      proxy_log("message size mismatch: expected %zu but got %u", size, hdr.size);
      return false;
   }

   /* Initialize fd count early - will be updated if fds are received */
   if (out_fd_count)
      *out_fd_count = 0;

   struct msghdr msg = {
      .msg_iov =
         &(struct iovec){
            .iov_base = data,
            .iov_len = size,
         },
      .msg_iovlen = 1,
   };

   char cmsg_buf[CMSG_SPACE(sizeof(*fds) * PROXY_SOCKET_MAX_FD_COUNT)];
   if (hdr.fd_count > 0 && max_fd_count > 0) {
      int expected_fds = hdr.fd_count < (uint32_t)max_fd_count ? hdr.fd_count : max_fd_count;
      msg.msg_control = cmsg_buf;
      msg.msg_controllen = CMSG_SPACE(sizeof(*fds) * expected_fds);

      struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
      memset(cmsg, 0, sizeof(*cmsg));
   }

   /* For SOCK_STREAM, we need to read exactly the specified size */
   size_t total_read = 0;
   while (total_read < size) {
      msg.msg_iov->iov_base = (char *)data + total_read;
      msg.msg_iov->iov_len = size - total_read;

      size_t chunk_size;
      if (!proxy_socket_recvmsg(socket, &msg, &chunk_size))
         return false;

      total_read += chunk_size;

      /* Only expect fds on first recv */
      if (msg.msg_control) {
         int received_fd_count;
         const int *received_fds = get_received_fds(&msg, &received_fd_count);
         if (received_fd_count > max_fd_count)
            received_fd_count = max_fd_count;

         if (fds)
            memcpy(fds, received_fds, sizeof(*fds) * received_fd_count);
         if (out_fd_count)
            *out_fd_count = received_fd_count;

         /* Clear control message for subsequent reads */
         msg.msg_control = NULL;
         msg.msg_controllen = 0;
      }
   }

   if (!fds && out_fd_count)
      *out_fd_count = 0;

   return true;
#else
   /* SOCK_SEQPACKET preserves message boundaries */
   struct msghdr msg = {
      .msg_iov =
         &(struct iovec){
            .iov_base = data,
            .iov_len = size,
         },
      .msg_iovlen = 1,
   };

   char cmsg_buf[CMSG_SPACE(sizeof(*fds) * PROXY_SOCKET_MAX_FD_COUNT)];
   if (max_fd_count) {
      assert(fds && max_fd_count <= PROXY_SOCKET_MAX_FD_COUNT);
      msg.msg_control = cmsg_buf;
      msg.msg_controllen = CMSG_SPACE(sizeof(*fds) * max_fd_count);

      struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
      memset(cmsg, 0, sizeof(*cmsg));
   }

   if (!proxy_socket_recvmsg(socket, &msg, NULL))
      return false;

   if (max_fd_count) {
      int received_fd_count;
      const int *received_fds = get_received_fds(&msg, &received_fd_count);
      assert(received_fd_count <= max_fd_count);

      memcpy(fds, received_fds, sizeof(*fds) * received_fd_count);
      *out_fd_count = received_fd_count;
   } else if (out_fd_count) {
      *out_fd_count = 0;
   }

   return true;
#endif
}

bool
proxy_socket_receive_reply(struct proxy_socket *socket, void *data, size_t size)
{
   return proxy_socket_receive_reply_internal(socket, data, size, NULL, 0, NULL);
}

bool
proxy_socket_receive_reply_with_fds(struct proxy_socket *socket,
                                    void *data,
                                    size_t size,
                                    int *fds,
                                    int max_fd_count,
                                    int *out_fd_count)
{
   return proxy_socket_receive_reply_internal(socket, data, size, fds, max_fd_count,
                                              out_fd_count);
}

static bool
proxy_socket_sendmsg(struct proxy_socket *socket, const struct msghdr *msg)
{
#ifdef __APPLE__
   /* On macOS with SOCK_STREAM, we may need to handle partial sends */
   assert(msg->msg_iovlen == 1);
   size_t total_sent = 0;
   size_t size = msg->msg_iov[0].iov_len;
   char *data = msg->msg_iov[0].iov_base;
   bool fds_sent = false;

   while (total_sent < size) {
      struct iovec iov = {
         .iov_base = data + total_sent,
         .iov_len = size - total_sent,
      };
      struct msghdr send_msg = {
         .msg_iov = &iov,
         .msg_iovlen = 1,
      };

      /* Only attach fds to the first send */
      if (!fds_sent && msg->msg_control) {
         send_msg.msg_control = msg->msg_control;
         send_msg.msg_controllen = msg->msg_controllen;
      }

      const ssize_t s = sendmsg(socket->fd, &send_msg, MSG_NOSIGNAL);
      if (unlikely(s < 0)) {
         if (errno == EAGAIN || errno == EINTR)
            continue;

         proxy_log("failed to send message: %s", strerror(errno));
         return false;
      }

      total_sent += s;
      if (msg->msg_control)
         fds_sent = true;
   }
   return true;
#else
   do {
      const ssize_t s = sendmsg(socket->fd, msg, MSG_NOSIGNAL);
      if (unlikely(s < 0)) {
         if (errno == EAGAIN || errno == EINTR)
            continue;

         proxy_log("failed to send message: %s", strerror(errno));
         return false;
      }

      /* no partial send since the socket type is SOCK_SEQPACKET */
      assert(msg->msg_iovlen == 1 && msg->msg_iov[0].iov_len == (size_t)s);
      return true;
   } while (true);
#endif
}

static bool
proxy_socket_send_request_internal(struct proxy_socket *socket,
                                   const void *data,
                                   size_t size,
                                   const int *fds,
                                   int fd_count)
{
   assert(data && size);

#ifdef __APPLE__
   /* On macOS with SOCK_STREAM, we use message framing:
    * 1. Write 8-byte header (size + fd_count)
    * 2. Send data + fds with sendmsg
    */
   struct stream_msg_header hdr = {
      .size = (uint32_t)size,
      .fd_count = (uint32_t)fd_count,
   };
   proxy_log("proxy_send_request: sending framing header size=%u fd_count=%u", hdr.size, hdr.fd_count);
   if (!proxy_socket_write_all(socket->fd, &hdr, sizeof(hdr)))
      return false;
#endif

   struct msghdr msg = {
      .msg_iov =
         &(struct iovec){
            .iov_base = (void *)data,
            .iov_len = size,
         },
      .msg_iovlen = 1,
   };

   char cmsg_buf[CMSG_SPACE(sizeof(*fds) * PROXY_SOCKET_MAX_FD_COUNT)];
   if (fd_count) {
      assert(fds && fd_count <= PROXY_SOCKET_MAX_FD_COUNT);
      msg.msg_control = cmsg_buf;
      msg.msg_controllen = CMSG_SPACE(sizeof(*fds) * fd_count);

      struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
      cmsg->cmsg_level = SOL_SOCKET;
      cmsg->cmsg_type = SCM_RIGHTS;
      cmsg->cmsg_len = CMSG_LEN(sizeof(*fds) * fd_count);
      memcpy(CMSG_DATA(cmsg), fds, sizeof(*fds) * fd_count);
   }

   proxy_log("proxy_send_request: sending data size=%zu", size);
   return proxy_socket_sendmsg(socket, &msg);
}

bool
proxy_socket_send_request(struct proxy_socket *socket, const void *data, size_t size)
{
   return proxy_socket_send_request_internal(socket, data, size, NULL, 0);
}

bool
proxy_socket_send_request_with_fds(struct proxy_socket *socket,
                                   const void *data,
                                   size_t size,
                                   const int *fds,
                                   int fd_count)
{
   return proxy_socket_send_request_internal(socket, data, size, fds, fd_count);
}
