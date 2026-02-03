#pragma once

#include "eph/core/ring_buffer.hpp"
#include "eph/core/shared_memory.hpp"
#include <chrono>
#include <optional>
#include <string>

namespace eph::ipc {

static constexpr size_t DEFAULT_CAPACITY = 1024;

/**
 * @brief IPC 消息发送端 (SPSC Queue Writer)
 *
 * @details
 * 基于 RingBuffer 的队列语义封装。
 * - **不可丢弃**: 不同于
 * Snapshot，这里的每个包都承载独立信息（如订单流），原则上不应覆盖。
 */
template <typename T, size_t Capacity = DEFAULT_CAPACITY>
  requires ShmData<T>
class Sender {
public:
  /**
   * @param name 共享内存名称
   * @param use_huge_pages 是否使用大页 (2MB/1GB)
   */
  explicit Sender(std::string name, bool use_huge_pages = false)
      : shm_(std::move(name), true, use_huge_pages) {}

  // --- 基础接口 ---
  void send(const T &data) { shm_->push(data); }
  [[nodiscard]] bool try_send(const T &data) { return shm_->try_push(data); }

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

  // --- 状态查询 ---
  size_t size() const noexcept { return shm_->size(); }
  bool is_full() const noexcept { return shm_->full(); }
  static constexpr size_t capacity() noexcept { return Capacity; }
  const std::string &name() const noexcept { return shm_.name(); }

private:
  SharedMemory<RingBuffer<T, Capacity>> shm_;
};

/**
 * @brief IPC 消息接收端 (SPSC Queue Reader)
 */
template <typename T, size_t Capacity = DEFAULT_CAPACITY>
  requires ShmData<T>
class Receiver {
public:
  /**
   * @param name 共享内存名称
   * @param use_huge_pages 是否使用大页 (2MB/1GB)。Receiver 必须与 Sender
   * 的大页配置一致
   */
  explicit Receiver(std::string name, bool use_huge_pages = false)
      : shm_(std::move(name), false, use_huge_pages) {}

  // --- 基础接口 ---
  T receive() { return shm_->pop(); }
  void receive(T &out) { shm_->pop(out); }
  [[nodiscard]] bool try_receive(T &out) { return shm_->try_pop(out); }
  std::optional<T> try_receive() { return shm_->try_pop(); }

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

  // --- 状态查询 ---
  size_t size() const noexcept { return shm_->size(); }
  bool is_empty() const noexcept { return shm_->empty(); }
  static constexpr size_t capacity() noexcept { return Capacity; }
  const std::string &name() const noexcept { return shm_.name(); }

private:
  SharedMemory<RingBuffer<T, Capacity>> shm_;
};

template <typename T, size_t Capacity = DEFAULT_CAPACITY>
auto channel(const std::string &name, bool use_huge_pages = false) {
  Sender<T, Capacity> sender(name, use_huge_pages);
  Receiver<T, Capacity> receiver(name, use_huge_pages);

  return std::make_pair(std::move(sender), std::move(receiver));
}

} // namespace eph::ipc
