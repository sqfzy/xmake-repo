#pragma once

#include <numa.h>
#include <pthread.h>
#include <sched.h>
#include <system_error>

// 平台特定的头文件包含
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||             \
    defined(_M_IX86)
#include <immintrin.h>
#endif

namespace eph {

namespace detail {
constexpr std::size_t CACHE_LINE_SIZE = 64;
constexpr size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024; // 2MB
}

/**
 * @brief CPU 自旋等待策略 (Hint 指令)
 *
 * @details
 * 在自旋锁 (Spinlock) 或 Busy Wait 循环中调用此函数至关重要：
 * 1. **流水线优化**: `pause` (x86) 指令告诉 CPU 这是一个循环等待，防止 CPU
 * 误判分支预测而清空流水线，减少退出循环时的性能惩罚。
 * 2. **功耗控制**: 降低 CPU 在自旋时的执行频率，减少发热和电能消耗。
 * 3. **超线程友好**: 在 Hyper-Threading
 * 架构上，让出执行单元给同一个核心上的另一个硬件线程。
 */
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||             \
    defined(_M_IX86)
  _mm_pause(); // 提示 CPU 这是一个自旋循环，降低功耗并避免流水线清空
#elif defined(__aarch64__)
  asm volatile("yield"); // ARM64
#else
  std::this_thread::yield(); // Fallback
#endif
}

/**
 * 设置当前线程为实时调度优先级
 */
inline void set_realtime_priority(int priority = 99) {
  sched_param param;
  param.sched_priority = priority;

  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "Failed to set SCHED_FIFO realtime priority");
  }
}

/**
 * 绑定 CPU 亲和性
 */
inline void bind_cpu(int core_id) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);

  if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "Failed to set CPU affinity");
  }
}

/**
 * 绑定 NUMA 节点与 CPU 亲和性
 */
inline void bind_numa(int node, int core_id) {
  // 1. 检查 NUMA 是否可用
  if (numa_available() < 0) {
    throw std::system_error(ENOTSUP, std::generic_category(),
                            "NUMA is not available on this system");
  }

  // 2. 校验 CPU 与节点的物理拓扑关系
  int actual_node = numa_node_of_cpu(core_id);
  if (actual_node != node) {
    throw std::system_error(
        EINVAL, std::generic_category(),
        "Topology mismatch: Core is not on the specified NUMA node");
  }

  // 3. 绑定内存策略
  struct bitmask *nodemask = numa_allocate_nodemask();
  if (!nodemask) {
    throw std::system_error(errno, std::generic_category(),
                            "Failed to allocate NUMA nodemask");
  }

  numa_bitmask_setbit(nodemask, node);
  numa_set_membind(nodemask);
  numa_free_nodemask(nodemask);

  bind_cpu(core_id);
}

} // namespace eph
