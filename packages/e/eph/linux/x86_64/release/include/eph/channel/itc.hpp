#pragma once

#include "eph/core/ring_buffer.hpp"
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <sys/mman.h>

namespace eph::itc {

static constexpr size_t DEFAULT_CAPACITY = 1024;

template <typename T, size_t Capacity = DEFAULT_CAPACITY >
  requires ShmData<T>
class Sender {
public:
  explicit Sender(std::shared_ptr<RingBuffer<T, Capacity>> buffer)
      : buffer_(std::move(buffer)) {}

  Sender(const Sender &) = delete;
  Sender &operator=(const Sender &) = delete;

  Sender(Sender &&) noexcept = default;
  Sender &operator=(Sender &&) noexcept = default;

  // --- 基础接口 ---
  void send(const T &data) { buffer_->push(data); }
  [[nodiscard]] bool try_send(const T &data) { return buffer_->try_push(data); }

  // --- 超时接口 ---
  template <class Rep, class Period>
  bool send(const T &data, const std::chrono::duration<Rep, Period> &timeout) {
    auto start = std::chrono::steady_clock::now();
    while (!try_send(data)) {
      if (std::chrono::steady_clock::now() - start > timeout)
        return false;
      cpu_relax();
    }
    return true;
  }

  template <class Clock, class Duration>
  bool send(const T &data,
            const std::chrono::time_point<Clock, Duration> &deadline) {
    while (!try_send(data)) {
      if (Clock::now() >= deadline)
        return false;
      cpu_relax();
    }
    return true;
  }

  // --- 批量接口 ---
  template <typename InputIt> size_t send_batch(InputIt first, InputIt last) {
    size_t count = 0;
    while (first != last) {
      if (!try_send(*first))
        break;
      ++first;
      ++count;
    }
    return count;
  }

  // --- 状态查询 ---
  size_t size() const noexcept { return buffer_->size(); }
  bool is_full() const noexcept { return buffer_->full(); }
  static constexpr size_t capacity() noexcept { return Capacity; }

private:
  std::shared_ptr<RingBuffer<T, Capacity>> buffer_;
};

template <typename T, size_t Capacity = DEFAULT_CAPACITY >
  requires ShmData<T>
class Receiver {
public:
  explicit Receiver(std::shared_ptr<RingBuffer<T, Capacity>> buffer)
      : buffer_(std::move(buffer)) {}

  Receiver(const Receiver &) = delete;
  Receiver &operator=(const Receiver &) = delete;

  Receiver(Receiver &&) noexcept = default;
  Receiver &operator=(Receiver &&) noexcept = default;

  // --- 基础接口 ---
  T receive() { return buffer_->pop(); }
  void receive(T &out) { buffer_->pop(out); }
  [[nodiscard]] bool try_receive(T &out) { return buffer_->try_pop(out); }
  std::optional<T> try_receive() { return buffer_->try_pop(); }

  // --- 超时接口 ---
  template <class Rep, class Period>
  bool receive(T &out, const std::chrono::duration<Rep, Period> &timeout) {
    auto start = std::chrono::steady_clock::now();
    while (!try_receive(out)) {
      if (std::chrono::steady_clock::now() - start > timeout)
        return false;
      cpu_relax();
    }
    return true;
  }

  template <class Clock, class Duration>
  std::optional<T>
  receive(const std::chrono::time_point<Clock, Duration> &deadline) {
    T out;
    while (!try_receive(out)) {
      if (Clock::now() >= deadline)
        return std::nullopt;
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
  size_t size() const noexcept { return buffer_->size(); }
  bool is_empty() const noexcept { return buffer_->empty(); }
  static constexpr size_t capacity() noexcept { return Capacity; }

private:
  std::shared_ptr<RingBuffer<T, Capacity>> buffer_;
};

template <typename T, size_t Capacity>
std::shared_ptr<RingBuffer<T, Capacity>> make_huge_ring_buffer() {
  using RB = RingBuffer<T, Capacity>;

  // 1. 计算对齐后的大小
  size_t raw_size = sizeof(RB);
  size_t map_size = align_up<detail::HUGE_PAGE_SIZE>(raw_size);

  // 2. 匿名映射 (MAP_ANONYMOUS)，不需要文件描述符，只存在于内存中
  void *ptr = mmap(nullptr, map_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

  if (ptr == MAP_FAILED) {
    throw std::system_error(
        errno, std::generic_category(),
        "mmap failed for huge pages (ITC). Check /proc/sys/vm/nr_hugepages");
  }

  // 3. 在这块内存上构造 RingBuffer
  RB *rb_ptr = static_cast<RB *>(ptr);
  std::construct_at(rb_ptr);

  // 4. 创建 shared_ptr，并挂载自定义删除器
  //    当引用计数归零时，Lambda 会被调用
  return std::shared_ptr<RB>(rb_ptr, [map_size](RB *p) {
    // a. 调用析构函数
    p->~RB();
    // b. 释放大页内存
    munmap(p, map_size);
  });
}

template <typename T, size_t Capacity = DEFAULT_CAPACITY >
auto channel(bool use_huge_pages = false) {
  std::shared_ptr<RingBuffer<T, Capacity>> buffer;

  if (use_huge_pages) {
    buffer = make_huge_ring_buffer<T, Capacity>();
  } else {
    buffer = std::make_shared<RingBuffer<T, Capacity>>();
  }

  return std::make_pair(Sender<T, Capacity>(buffer),
                        Receiver<T, Capacity>(buffer));
}

} // namespace eph::itc
