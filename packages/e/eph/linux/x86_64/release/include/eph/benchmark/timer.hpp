#pragma once

#include <cassert>
#include <chrono>
#include <cstdint>
#include <print>
#include <string_view>

// 平台检测
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace eph::benchmark {

class TSC {
public:
  // 强制内联宏
#if defined(_MSC_VER)
#define ALWAYS_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define ALWAYS_INLINE inline
#endif

  // 单例访问
  static TSC &global() {
    static TSC instance;
    return instance;
  }

  [[nodiscard]] static ALWAYS_INLINE uint64_t now() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned int aux;
    return __rdtscp(&aux);
#elif defined(__aarch64__) || defined(_M_ARM64)
    uint64_t val;
    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(val)::"memory");
    return val;
#else
#error                                                                         \
    "Current architecture does not support hardware TSC. Use std::chrono instead."
#endif
  }

  void
  init(std::chrono::milliseconds duration = std::chrono::milliseconds(200)) {
    std::print("[Timer] Calibrating... ");

    // 1. 预热
    auto start_warm = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start_warm <
           std::chrono::milliseconds(20)) {
      _mm_pause();
    }

    // 2. 采样
    auto t1 = std::chrono::steady_clock::now();
    uint64_t c1 = now();

    // 忙等待
    while (std::chrono::steady_clock::now() - t1 < duration) {
      _mm_pause();
    }

    uint64_t c2 = now();
    auto t2 = std::chrono::steady_clock::now();

    // 3. 计算倍率
    double ns_total = std::chrono::duration<double, std::nano>(t2 - t1).count();
    double cycles_total = static_cast<double>(c2 - c1);

    ns_per_cycle_ = ns_total / cycles_total;

    std::print("[Timer] Cpu Frequency: {:.2f} GHz\n", 1.0 / ns_per_cycle_);
  }

  // Cycles -> Nanoseconds
  [[nodiscard]] double to_ns(uint64_t cycles) const noexcept {
    if (ns_per_cycle_ <= 0.0) [[unlikely]] {
      std::println(stderr, "FATAL: Benchmark used before TSC::init()!");
      std::terminate();
    }
    return cycles * ns_per_cycle_;
  }

private:
  TSC() = default;
  double ns_per_cycle_ = 0.0;
};

// =========================================================
// 作用域计时器
// =========================================================
class ScopedTimer {
public:
  explicit ScopedTimer(std::string_view name)
      : name_(name), start_(TSC::now()) {}

  ~ScopedTimer() {
    uint64_t end = TSC::now();
    uint64_t diff = end - start_;

    double ns = TSC::global().to_ns(diff);

    // 自动缩放单位
    if (ns < 1000.0) {
      std::println("[{}] {:.2f} ns", name_, ns);
    } else if (ns < 1'000'000.0) {
      std::println("[{}] {:.3f} us", name_, ns / 1000.0);
    } else if (ns < 1'000'000'000.0) {
      std::println("[{}] {:.3f} ms", name_, ns / 1'000'000.0);
    } else {
      std::println("[{}] {:.4f} s", name_, ns / 1'000'000'000.0);
    }
  }

private:
  std::string_view name_;
  uint64_t start_;
};

#define BENCH_SCOPE(name) ::benchmark::ScopedTimer _timer_##__LINE__(name)

} // namespace benchmark
