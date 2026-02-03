#pragma once

#include "eph/platform.hpp"
#include "eph/types.hpp"
#include <array>
#include <atomic>
#include <optional>
#include <bit>

namespace eph {

/**
 * @brief 单生产者-单消费者 (SPSC) 无锁环形缓冲区 (支持影子索引优化)
 *
 * @details
 * **核心机制：**
 * 使用 Head 和 Tail 两个索引控制读写。
 * 引入 **影子索引 (Shadow Indices)**：
 * - Producer 缓存一份 `shadow_head_`，仅在缓冲区看似已满时更新。
 * - Consumer 缓存一份 `shadow_tail_`，仅在缓冲区看似为空时更新。
 *
 * **内存布局与伪共享 (False Sharing) 防护：**
 * 严格按照“写入者”归类，将变量隔离在不同的缓存行 (Cache Line)。
 *
 * [ head_ (8B) | shadow_tail_ (8B) ... padding ... ] <--- Consumer Line (Consumer 写)
 * [ tail_ (8B) | shadow_head_ (8B) ... padding ... ] <--- Producer Line (Producer 写)
 * [ buffer_ ...                                    ] <--- Data Lines
 *
 * @tparam T 数据类型，必须是 TriviallyCopyable (POD)。
 * @tparam Capacity 容量，必须是 2 的幂。
 */
template <typename T, size_t Capacity>
  requires ShmData<T>
class RingBuffer {
  static_assert(std::has_single_bit(Capacity), "Capacity must be power of 2");
  static constexpr size_t mask_ = Capacity - 1;

  // ===========================================================================
  // 缓存行隔离结构
  // ===========================================================================

  // 消费者缓存行：Consumer 频繁写入，Producer 偶尔读取 head_
  struct alignas(detail::CACHE_LINE_SIZE) ConsumerLine {
    std::atomic<size_t> head_{0};
    size_t shadow_tail_{0}; // 消费者本地缓存的 tail，减少对 atomic tail_ 的读取
  };

  // 生产者缓存行：Producer 频繁写入，Consumer 偶尔读取 tail_
  struct alignas(detail::CACHE_LINE_SIZE) ProducerLine {
    std::atomic<size_t> tail_{0};
    size_t shadow_head_{0}; // 生产者本地缓存的 head，减少对 atomic head_ 的读取
  };

  ConsumerLine consumer_;
  ProducerLine producer_;

  // 数据区对齐
  static constexpr size_t BufferAlign = (alignof(T) > detail::CACHE_LINE_SIZE)
                                            ? alignof(T)
                                            : detail::CACHE_LINE_SIZE;

  alignas(BufferAlign) std::array<T, Capacity> buffer_;

  // ===========================================================================
  // 内核 (Kernels) - 单次原子操作
  // ===========================================================================

  template <typename F> bool raw_produce(F &&writer) noexcept {
    const size_t tail = producer_.tail_.load(std::memory_order_relaxed);
    
    // 1. 快速路径：使用影子索引检查是否有空间
    // shadow_head_ 是 head_ 的历史快照，一定 <= 实际 head_。
    // 如果 tail - shadow_head_ < Capacity，说明实际空间肯定足够。
    if (tail - producer_.shadow_head_ >= Capacity) {
      
      // 2. 慢速路径：影子索引认为满了，重新加载实际 head_ (Acquire)
      // 这一步会产生跨核流量
      const size_t head = consumer_.head_.load(std::memory_order_acquire);
      producer_.shadow_head_ = head; // 更新影子

      // 3. 再次检查真实状态
      if (tail - head >= Capacity) {
        return false; // Full
      }
    }

    // 完美转发 writer，直接在 SHM 内存上操作
    std::forward<F>(writer)(buffer_[tail & mask_]);

    producer_.tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  template <typename F> bool raw_consume(F &&visitor) noexcept {
    const size_t head = consumer_.head_.load(std::memory_order_relaxed);

    // 1. 快速路径：使用影子索引检查是否有数据
    // shadow_tail_ 是 tail_ 的历史快照，一定 <= 实际 tail_。
    // 如果 shadow_tail_ > head，说明实际 tail_ 肯定 > head (有数据)。
    if (consumer_.shadow_tail_ == head) { // 等于意味着影子视角为空
      
      // 2. 慢速路径：影子索引认为空了，重新加载实际 tail_ (Acquire)
      // 这一步会产生跨核流量
      const size_t tail = producer_.tail_.load(std::memory_order_acquire);
      consumer_.shadow_tail_ = tail; // 更新影子

      // 3. 再次检查真实状态
      if (head == tail) {
        return false; // Empty
      }
    }

    std::forward<F>(visitor)(buffer_[head & mask_]);

    consumer_.head_.store(head + 1, std::memory_order_release);
    return true;
  }

public:
  RingBuffer() noexcept {
    // 初始化 atomic 变量
    consumer_.head_.store(0, std::memory_order_relaxed);
    producer_.tail_.store(0, std::memory_order_relaxed);
    // 影子变量初始化
    consumer_.shadow_tail_ = 0;
    producer_.shadow_head_ = 0;
  }

  // ===========================================================================
  // PUSH 操作 (非阻塞 / 阻塞)
  // ===========================================================================

  // PERF: 尝试零拷贝写入
  template <typename F> bool try_produce(F &&writer) noexcept {
    return raw_produce(std::forward<F>(writer));
  }

  // PERF: 尝试 T 拷贝赋值
  bool try_push(const T &data) noexcept {
    return raw_produce([&data](T &slot) { slot = data; });
  }

  // PERF: 尝试 T 移动赋值
  bool try_push(T &&data) noexcept {
    return raw_produce([&data](T &slot) { slot = std::move(data); });
  }

  // PERF: 阻塞式零拷贝写入
  template <typename F> void produce(F &&writer) noexcept {
    while (!raw_produce(writer)) {
      cpu_relax();
    }
  }

  // PERF: 阻塞式 T 拷贝赋值
  void push(const T &data) noexcept {
    produce([&](T &slot) { slot = data; });
  }

  // PERF: 阻塞式 T 移动赋值
  void push(T &&data) noexcept {
    produce([&](T &slot) { slot = std::move(data); });
  }

  // ===========================================================================
  // POP 操作 (非阻塞 / 阻塞)
  // ===========================================================================

  // PERF: 尝试零拷贝读取
  template <typename F> bool try_consume(F &&visitor) noexcept {
    return raw_consume(std::forward<F>(visitor));
  }

  // PERF: 尝试 T 拷贝赋值 (复用内存)
  bool try_pop(T &out) noexcept {
    return raw_consume([&out](const T &data) { out = data; });
  }

  // PERF: 尝试 T 拷贝构造 (寄存器返回)
  std::optional<T> try_pop() noexcept {
    std::optional<T> res;
    if (raw_consume([&res](const T &data) { res.emplace(data); })) {
      return res;
    }
    return std::nullopt;
  }

  // PERF: 阻塞式零拷贝读取
  template <typename F> void consume(F &&visitor) noexcept {
    while (!raw_consume(visitor)) {
      cpu_relax();
    }
  }

  // PERF: 阻塞式 T 拷贝赋值
  void pop(T &out) noexcept {
    consume([&out](const T &data) { out = data; });
  }

  // PERF: 阻塞式 T 值返回
  T pop() noexcept {
    std::optional<T> res;
    consume([&res](const T &data) { res.emplace(data); });
    return *res;
  }

  // ---------------------------------------------------------------------------
  // 状态查询
  // ---------------------------------------------------------------------------
  
  size_t size() const noexcept {
    auto tail = producer_.tail_.load(std::memory_order_relaxed);
    auto head = consumer_.head_.load(std::memory_order_relaxed);
    return tail - head;
  }

  bool empty() const noexcept { return size() == 0; }
  bool full() const noexcept { return size() >= Capacity; }
  static constexpr size_t capacity() noexcept { return Capacity; }
};

} // namespace eph
