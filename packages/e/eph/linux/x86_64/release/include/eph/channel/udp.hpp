#pragma once

#include "eph/platform.hpp"
#include "eph/core/socket.hpp"
#include "eph/types.hpp"
#include <chrono>
#include <optional>
#include <string>

namespace eph::udp {

using namespace eph::detail;

using eph::imc::Socket;

static constexpr size_t DEFAULT_CAPACITY = 1024;

template <typename T, size_t Capacity = DEFAULT_CAPACITY>
  requires ShmData<T>
class Sender {
public:
  Sender(std::string ip, uint16_t port) : socket_(SOCK_DGRAM) {
    socket_.connect(ip, port);

    // 设置内核发送缓冲区
    int sndbuf = static_cast<int>(Capacity * sizeof(T));
    socket_.set_opt(SOL_SOCKET, SO_SNDBUF, sndbuf);
  }

  // --- 基础接口 ---
  void send(const T &data) {
    while (!try_send(data)) {
      cpu_relax();
    }
  }

  [[nodiscard]] bool try_send(const T &data) {
    ssize_t ret = socket_.send(&data, sizeof(T));
    return ret == static_cast<ssize_t>(sizeof(T));
  }

  // --- 超时接口 ---
  template <class Rep, class Period>
  bool send(const T &data, const std::chrono::duration<Rep, Period> &timeout) {
    auto start = std::chrono::steady_clock::now();
    while (!try_send(data)) {
      if (std::chrono::steady_clock::now() - start > timeout) {
        return false;
      }
      cpu_relax();
    }
    return true;
  }

  template <class Clock, class Duration>
  bool send(const T &data,
            const std::chrono::time_point<Clock, Duration> &deadline) {
    while (!try_send(data)) {
      if (Clock::now() >= deadline) {
        return false;
      }
      cpu_relax();
    }
    return true;
  }

  // --- 批量接口 ---
  template <typename InputIt> size_t send_batch(InputIt first, InputIt last) {
    size_t count = 0;
    while (first != last) {
      if (!try_send(*first)) {
        break;
      }
      ++first;
      ++count;
    }
    return count;
  }

  size_t size() const noexcept { return 0; }
  bool is_full() const noexcept { return false; }
  static constexpr size_t capacity() noexcept { return Capacity; }

private:
  Socket socket_;
};

template <typename T, size_t Capacity = DEFAULT_CAPACITY>
  requires ShmData<T>
class Receiver {
public:
  explicit Receiver(uint16_t port, std::string ip = "0.0.0.0")
      : socket_(SOCK_DGRAM) {
    // 统一使用 set_opt 进行配置
    int on = 1;
    socket_.set_opt(SOL_SOCKET, SO_REUSEADDR, on);
    socket_.set_opt(SOL_SOCKET, SO_REUSEPORT, on);

    socket_.bind(ip, port);

    // 设置内核接收缓冲区
    int rcvbuf = static_cast<int>(Capacity * sizeof(T));
    if (rcvbuf < 262144) { // 至少 256KB
      rcvbuf = 262144;
    }
    socket_.set_opt(SOL_SOCKET, SO_RCVBUF, rcvbuf);
  }

  // --- 基础接口 ---
  T receive() { return pop(); }
  void receive(T &out) { pop(out); }

  [[nodiscard]] bool try_receive(T &out) {
    ssize_t ret = socket_.recv(&out, sizeof(T));
    return ret == static_cast<ssize_t>(sizeof(T));
  }

  std::optional<T> try_receive() {
    T out;
    if (try_receive(out)) {
      return out;
    }
    return std::nullopt;
  }

  T pop() {
    T out;
    while (!try_receive(out)) {
      cpu_relax();
    }
    return out;
  }

  void pop(T &out) {
    while (!try_receive(out)) {
      cpu_relax();
    }
  }

  // --- 超时接口 ---
  template <class Rep, class Period>
  bool receive(T &out, const std::chrono::duration<Rep, Period> &timeout) {
    auto start = std::chrono::steady_clock::now();
    while (!try_receive(out)) {
      if (std::chrono::steady_clock::now() - start > timeout) {
        return false;
      }
      cpu_relax();
    }
    return true;
  }

  template <class Clock, class Duration>
  std::optional<T>
  receive(const std::chrono::time_point<Clock, Duration> &deadline) {
    T out;
    while (!try_receive(out)) {
      if (Clock::now() >= deadline) {
        return std::nullopt;
      }
      cpu_relax();
    }
    return out;
  }

  // --- 批量接口 ---
  template <typename OutputIt>
  size_t receive_batch(OutputIt d_first, size_t max_count) {
    size_t count = 0;
    T temp;
    while (count < max_count && try_receive(temp)) {
      *d_first++ = temp;
      ++count;
    }
    return count;
  }

  size_t size() const noexcept { return 0; }
  bool is_empty() const noexcept { return false; }
  static constexpr size_t capacity() noexcept { return Capacity; }

private:
  Socket socket_;
};
} // namespace eph::udp
