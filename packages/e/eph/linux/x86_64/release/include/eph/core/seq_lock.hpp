#pragma once

#include "eph/platform.hpp"
#include "eph/types.hpp"
#include <atomic>


namespace eph {

using namespace eph::detail;

/**
 * @brief 单生产者-多消费者 (SPMC) 顺序锁快照容器
 *
 * 特性：
 * 1. Writer 永远不阻塞 (Wait-free)。
 * 2. Reader 只取最新数据，若读取期间发生写入则重试 (Lock-free)。
 * 3. 适用于 "Conflation" 场景：只关心最新状态，允许丢弃旧数据。
 *
 * 内存布局：
 * [ seq (8B) ] [ padding ]
 * [ data (T) ...         ]
 */
template <typename T>
  requires ShmData<T>
class alignas(alignof(T) > CACHE_LINE_SIZE ? alignof(T)
                                                   : CACHE_LINE_SIZE)
    SeqLock {
  static_assert(std::atomic<uint64_t>::is_always_lock_free,
                "SeqLock requires lock-free std::atomic<uint64_t>");

private:
  // 版本号：偶数=空闲，奇数=正在写入
  alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> seq_{0};

  alignas(alignof(T) > CACHE_LINE_SIZE
              ? alignof(T)
              : CACHE_LINE_SIZE) T data_;

public:
  SeqLock() noexcept = default;

  // ===========================================================================
  // Writer 操作 (Wait-free)
  // ===========================================================================

  // PERF: 零拷贝写入 (直接在共享内存上构造/修改)
  // F: void(T& slot)
  template <typename F> void write(F &&writer) noexcept {
    uint64_t seq = seq_.load(std::memory_order_relaxed);

    // 开始写入：序列号+1（变奇数）
    seq_.store(seq + 1, std::memory_order_relaxed);

    // Store-Store 屏障。确保序列号写入对读者可见后再写数据
    std::atomic_thread_fence(std::memory_order_release);

    // 写入数据
    std::forward<F>(writer)(data_);

    // Store-Store 屏障。确保数据写入完成后再更新序列号
    std::atomic_thread_fence(std::memory_order_release);

    // 完成写入：序列号+1（变偶数）
    seq_.store(seq + 2, std::memory_order_relaxed);
  }

  // PERF: 值拷贝写入
  void store(const T &val) noexcept {
    write([&val](T &slot) { slot = val; });
  }

  // ===========================================================================
  // Reader 操作 (Lock-free / Spin)
  // ===========================================================================

  // PERF: 尝试零拷贝读取 (Visitor 模式)
  // 如果读取期间数据发生变化，返回 false
  // F: void(const T& data)
  template <typename F> bool try_read(F &&visitor) const noexcept {
    // 读取开始版本号
    uint64_t seq0 = seq_.load(std::memory_order_relaxed);

    // 如果是奇数，说明正在写，数据是脏的
    if (seq0 & 1) {
      return false;
    }

    // Load-Load 屏障。确保先读序列号，再读数据
    std::atomic_thread_fence(std::memory_order_acquire);

    // 乐观读取数据
    // 在 C++ 内存模型中，如果此时有写者在写，严格来说是 Data Race (UB)，
    // 但 ShmData<T> 保证 T 是 TriviallyCopyable，
    // 所以即使 UB 也不会 crash，只是数据无意义。
    std::forward<F>(visitor)(data_);

    // Load-Load 屏障。确保数据读取完成后再读序列号
    std::atomic_thread_fence(std::memory_order_acquire);

    // 再次读取序列号
    uint64_t seq1 = seq_.load(std::memory_order_relaxed);

    // 验证一致性
    return seq0 == seq1;
  }

  // PERF: 尝试值拷贝读取
  bool try_load(T &out) const noexcept {
    return try_read([&out](const T &slot) { out = slot; });
  }

  // PERF: 阻塞式零拷贝读取
  template <typename F> void read(F &&visitor) const noexcept {
    while (!try_read(visitor)) {
      cpu_relax();
    }
  }

  // PERF: 阻塞式值拷贝读取
  T load() const noexcept {
    T out;
    read([&out](const T &slot) { out = slot; });
    return out;
  }

  // ===========================================================================
  // 状态查询
  // ===========================================================================

  // 粗略检查是否正在被写入
  bool may_busy() const noexcept {
    return seq_.load(std::memory_order_relaxed) & 1;
  }
};

} // namespace eph
