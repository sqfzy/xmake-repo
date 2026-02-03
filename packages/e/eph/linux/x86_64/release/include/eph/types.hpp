#pragma once

#include <atomic>

namespace eph {

/**
 * @brief [数据约束] 共享内存数据类型 Concept
 *
 * 存放在 RingBuffer 或 SeqLock 中的元素 T 必须满足：
 * 1. **TriviallyCopyable**: 保证可以用 `memcpy`
 * 安全复制，不包含自定义拷贝逻辑。
 * 2. **无指针**: (隐含) 必须是 Self-contained
 * 的数据，不能持有指向进程私有堆内存的指针。
 */
template <typename T>
concept ShmData =
    std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>;

/**
 * @brief [容器约束] 共享内存容器布局 Concept
 *
 * 约束 RingBuffer/SeqLock 等容器结构：
 * 1. **Standard Layout**: 内存布局由标准严格定义，确保 C++ 编译器不会插入奇怪的
 * Padding，保证跨进程兼容性。
 */
template <typename T>
concept ShmLayout =
    std::is_standard_layout_v<T> && std::is_default_constructible_v<T>;

// 检查原子操作在共享内存中是否免锁 (Linux x86_64 通常是 true)
// 如果不是免锁的，原子变量可能使用进程本地的哈希表锁，导致无法跨进程同步
template <typename T>
concept LockFreeAtomic = std::atomic<T>::is_always_lock_free;

template <size_t Alignment> inline size_t align_up(size_t size) {
  static_assert(Alignment > 0 && (Alignment & (Alignment - 1)) == 0,
                "Alignment must be a power of 2");
  return (size + Alignment - 1) & ~(Alignment - 1);
}

} // namespace eph
