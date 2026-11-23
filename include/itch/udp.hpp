#pragma once

// Minimal cross-platform UDP socket (Winsock on Windows, BSD sockets elsewhere)
// with multicast join, receive timeouts, and large receive buffers.

#include <cstdint>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using socket_t = int;
#endif

namespace itch::net {

struct NetInit {
  NetInit() {
#if defined(_WIN32)
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
#endif
  }
  ~NetInit() {
#if defined(_WIN32)
    WSACleanup();
#endif
  }
};

inline sockaddr_in make_addr(const std::string& host, std::uint16_t port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  inet_pton(AF_INET, host.c_str(), &a.sin_addr);
  return a;
}

inline bool is_multicast(const std::string& host) {
  in_addr a{};
  if (inet_pton(AF_INET, host.c_str(), &a) != 1) return false;
  const std::uint32_t h = ntohl(a.s_addr);
  return (h >> 28) == 0xE;
}

class UdpSocket {
 public:
  UdpSocket() { fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); }
  ~UdpSocket() { close(); }
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  bool ok() const {
#if defined(_WIN32)
    return fd_ != INVALID_SOCKET;
#else
    return fd_ >= 0;
#endif
  }

  void close() {
    if (!ok()) return;
#if defined(_WIN32)
    closesocket(fd_);
    fd_ = INVALID_SOCKET;
#else
    ::close(fd_);
    fd_ = -1;
#endif
  }

  bool reuse_addr() {
    int one = 1;
    return setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one) == 0;
  }

  bool recv_buffer(int bytes) {
    return setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes), sizeof bytes) == 0;
  }

  bool bind(const std::string& host, std::uint16_t port) {
    sockaddr_in a = make_addr(host, port);
    return ::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
  }

  bool join(const std::string& group, const std::string& iface = "0.0.0.0") {
    ip_mreq m{};
    inet_pton(AF_INET, group.c_str(), &m.imr_multiaddr);
    inet_pton(AF_INET, iface.c_str(), &m.imr_interface);
    return setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&m), sizeof m) == 0;
  }

  bool multicast_loopback(bool on, int ttl = 1) {
    const char loop = on ? 1 : 0;
    const int t = ttl;
    return setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop) == 0 &&
           setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&t), sizeof t) == 0;
  }

  bool recv_timeout_ms(int ms) {
#if defined(_WIN32)
    DWORD t = static_cast<DWORD>(ms);
    return setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t) == 0;
#else
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    return setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0;
#endif
  }

  // Returns bytes received, 0 on timeout, -1 on error.
  int recv_from(char* buf, int cap, sockaddr_in* from = nullptr) {
    sockaddr_in tmp{};
    socklen_t len = sizeof tmp;
    const int n = static_cast<int>(::recvfrom(fd_, buf, cap, 0, reinterpret_cast<sockaddr*>(&tmp), &len));
    if (from) *from = tmp;
    if (n >= 0) return n;
#if defined(_WIN32)
    const int e = WSAGetLastError();
    return (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) ? 0 : -1;
#else
    return 0;  // EAGAIN / EWOULDBLOCK on timeout
#endif
  }

  bool send_to(const char* buf, int len, const sockaddr_in& to) {
    return ::sendto(fd_, buf, len, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to) == len;
  }

 private:
#if defined(_WIN32)
  socket_t fd_ = INVALID_SOCKET;
#else
  socket_t fd_ = -1;
#endif
};

}  // namespace itch::net
