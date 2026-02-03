#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace eph::imc {

class Socket {
  int fd_ = -1;

public:
  Socket() = default;

  explicit Socket(int type) {
    if ((fd_ = ::socket(AF_INET, type, 0)) < 0)
      throw_err("socket");

    // 1. 设为非阻塞 (Non-blocking)
    set_non_blocking(true);

    // 2. IP 层优先权 (ToS): 告诉交换机/路由器这是一个急件
    int tos = IPTOS_LOWDELAY;
    set_opt(IPPROTO_IP, IP_TOS, tos);

    // 3. 内核忙轮询 (Busy Poll)
#ifdef SO_BUSY_POLL
    int busy_poll_us = 50;
    set_opt(SOL_SOCKET, SO_BUSY_POLL, busy_poll_us);
#endif

    // 4. TCP 优化
    // 禁用 Nagle: 有数据立即发，绝不等待凑包
    if (type == SOCK_STREAM) {
      int on = 1;
      set_opt(IPPROTO_TCP, TCP_NODELAY, on);
    }
  }

  Socket(Socket &&o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
  Socket &operator=(Socket &&o) noexcept {
    if (this != &o) {
      close();
      fd_ = std::exchange(o.fd_, -1);
    }
    return *this;
  }

  ~Socket() { close(); }

  void close() noexcept {
    if (fd_ != -1)
      ::close(std::exchange(fd_, -1));
  }

  explicit operator bool() const noexcept { return fd_ != -1; }
  [[nodiscard]] int fd() const noexcept { return fd_; }

  // --------------------------------------------------------------------------
  // 配置接口
  // --------------------------------------------------------------------------

  // 通用 setsockopt 封装
  // 返回 0 表示成功，-1 表示失败（可以通过 errno 查看原因）
  template <typename T> int set_opt(int level, int optname, const T &optval) {
    return ::setsockopt(fd_, level, optname, &optval, sizeof(optval));
  }

  // 非阻塞设置
  void set_non_blocking(bool on = true) {
    int f = ::fcntl(fd_, F_GETFL);
    ::fcntl(fd_, F_SETFL, on ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK));
  }

  // --------------------------------------------------------------------------
  // 核心 IO
  // --------------------------------------------------------------------------

  void bind(const std::string &ip, uint16_t port) {
    auto addr = make_addr(ip, port);
    if (::bind(fd_, (sockaddr *)&addr, sizeof(addr)) < 0)
      throw_err("bind");
  }

  void connect(const std::string &ip, uint16_t port) {
    auto addr = make_addr(ip, port);
    // 因为默认开了非阻塞，所以这里要处理 EINPROGRESS
    if (::connect(fd_, (sockaddr *)&addr, sizeof(addr)) < 0 &&
        errno != EINPROGRESS)
      throw_err("connect");
  }

  ssize_t send(std::span<const std::byte> buf, int flags = 0) const {
    return ::send(fd_, buf.data(), buf.size(), flags | MSG_NOSIGNAL);
  }
  ssize_t send(const void *buf, size_t len, int flags = 0) const {
    return ::send(fd_, buf, len, flags | MSG_NOSIGNAL);
  }

  ssize_t recv(std::span<std::byte> buf, int flags = 0) const {
    return ::recv(fd_, buf.data(), buf.size(), flags);
  }
  ssize_t recv(void *buf, size_t len, int flags = 0) const {
    return ::recv(fd_, buf, len, flags);
  }

  ssize_t sendto(std::span<const std::byte> buf, const std::string &ip,
                 uint16_t port) {
    auto addr = make_addr(ip, port);
    return ::sendto(fd_, buf.data(), buf.size(), MSG_NOSIGNAL,
                    (sockaddr *)&addr, sizeof(addr));
  }

private:
  static sockaddr_in make_addr(const std::string &ip, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0)
      throw std::runtime_error("Invalid IP: " + ip);
    return addr;
  }

  static void throw_err(const char *msg) {
    throw std::system_error(errno, std::system_category(), msg);
  }
};

}
