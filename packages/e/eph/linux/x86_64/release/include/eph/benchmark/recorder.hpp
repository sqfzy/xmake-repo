#pragma once

#include "timer.hpp"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <print>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace eph::benchmark {

// =========================================================
// 轻量级 HdrHistogram 实现 (Header-only)
// =========================================================
// 精度：3 位有效数字 (sub_bucket_count = 2048)
// 范围：1 到 2^63
class SimpleHdrHistogram {
public:
  SimpleHdrHistogram() {
    // 预分配内存，确保运行时无分配
    // 64 levels * 2048 buckets * 4 bytes ≈ 512 KB
    counts_.resize(kBucketSize * 64, 0);
  }

  void record(uint64_t value) {
    if (value == 0) return; // 忽略 0 延迟
    size_t idx = get_index(value);
    if (idx < counts_.size()) {
      counts_[idx]++;
    }
    total_count_++;
  }

  void reset() {
    std::fill(counts_.begin(), counts_.end(), 0);
    total_count_ = 0;
  }

  [[nodiscard]] uint64_t get_value_at_percentile(double percentile) const {
    if (total_count_ == 0) return 0;

    double target_count = total_count_ * (percentile / 100.0);
    uint64_t current_count = 0;

    for (size_t i = 0; i < counts_.size(); ++i) {
      if (counts_[i] > 0) {
        current_count += counts_[i];
        if (current_count >= target_count) {
          return get_value_from_index(i);
        }
      }
    }
    return get_max_value_recorded();
  }

  [[nodiscard]] uint64_t get_max_value_recorded() const {
    for (size_t i = counts_.size() - 1; i > 0; --i) {
      if (counts_[i] > 0) return get_value_from_index(i);
    }
    return 0;
  }

  // 迭代器接口，用于导出数据
  template <typename Func>
  void for_each_recorded_value(Func func) const {
    for (size_t i = 0; i < counts_.size(); ++i) {
      if (counts_[i] > 0) {
        func(get_value_from_index(i), counts_[i]);
      }
    }
  }

private:
  static constexpr int kSubBucketBits = 11; // 2^11 = 2048
  static constexpr int kBucketSize = 1 << kSubBucketBits;
  static constexpr uint64_t kSubBucketMask = kBucketSize - 1;

  std::vector<uint32_t> counts_;
  uint64_t total_count_ = 0;

  // 将数值映射到数组索引
  [[nodiscard]] static size_t get_index(uint64_t value) {
    if (value < kBucketSize) {
      return value;
    }
    // 找到最高有效位 (Magnitude)
    int magnitude = std::bit_width(value) - 1;
    // 计算该量级内的偏移
    int shift = magnitude - kSubBucketBits;
    // 索引 = (量级偏移) + (子桶偏移)
    // 量级偏移需要减去基础的 sub_bucket_bits，因为前 kBucketSize 个数直接存
    size_t magnitude_base = (magnitude - kSubBucketBits + 1) << kSubBucketBits;
    size_t sub_bucket = (value >> shift) & kSubBucketMask;
    
    return magnitude_base + sub_bucket;
  }

  // 将数组索引还原为数值（近似值）
  [[nodiscard]] static uint64_t get_value_from_index(size_t index) {
    if (index < kBucketSize) {
      return index;
    }
    
    size_t magnitude_idx = index >> kSubBucketBits;
    size_t sub_bucket = index & kSubBucketMask;
    
    int magnitude = magnitude_idx + kSubBucketBits - 1;
    int shift = magnitude - kSubBucketBits;
    
    uint64_t value = (static_cast<uint64_t>(1) << magnitude) + (sub_bucket << shift);
    return value;
  }
};

class Recorder {
public:
  explicit Recorder(std::string name) : name_(std::move(name)) {
    // Histogram 已经在构造函数中分配好内存
  }

  // =========================================================
  // 记录数据
  // =========================================================
  // 输入单位：Cycles
  void record(double cycles) {
    count_++;
    total_cycles_ += cycles;

    if (cycles < min_cycles_)
      min_cycles_ = cycles;
    if (cycles > max_cycles_)
      max_cycles_ = cycles;

    // HdrHistogram 记录 (转换为整数 Cycles)
    histogram_.record(static_cast<uint64_t>(cycles));
  }

  // =========================================================
  // 报告统计数据：控制台打印
  // =========================================================
  void print_report() const {
    if (count_ == 0) {
      std::println("[{}] No data recorded.", name_);
      return;
    }

    std::string time_str = get_current_time_str();
    std::string title = std::format(" BENCHMARK REPORT ({}) ", time_str);
    Stats stats = compute_stats_ns();

    constexpr int w_name = 40;
    constexpr int w_count = 10;
    constexpr int w_data = 12;

    constexpr int total_w = w_name + w_count + (w_data * 5) + 18;

    std::println("\n{:-^{}}", title, total_w);
    std::println("{:<{}} | {:>{}} | {:>{}} | {:>{}} | {:>{}} | {:>{}} | {:>{}}",
                 "Name", w_name, "Count", w_count, "Avg(ns)", w_data, "Min(ns)",
                 w_data, "P50(ns)", w_data, "P99(ns)", w_data, "Max(ns)",
                 w_data);

    std::println("{:-^{}}", "", total_w);

    std::println("{:<{}} | {:>{}} | {:>{}.2f} | {:>{}.2f} | {:>{}.2f} | "
                 "{:>{}.2f} | {:>{}.2f}",
                 stats.name, w_name, stats.count, w_count, stats.avg_ns, w_data,
                 stats.min_ns, w_data, stats.p50_ns, w_data, stats.p99_ns,
                 w_data, stats.max_ns, w_data);

    std::println("{:-^{}}\n", "", total_w);
  }

  // =========================================================
  // 导出统计数据：JSON
  // =========================================================
  void export_json(const std::string &output_dir = "outputs") const {
    ensure_directory(output_dir);
    Stats stats = compute_stats_ns();
    std::string time_str = get_current_time_str();

    std::string filename = sanitize_filename(name_) + "_" + time_str + ".json";
    fs::path path = fs::path(output_dir) / filename;

    std::ofstream file(path);
    if (!file.is_open())
      return;

    file << std::format(R"({{
  "name": "{}",
  "report_time": "{}",
  "count": {},
  "stats": {{
    "avg_ns": {:.2f},
    "min_ns": {:.2f},
    "max_ns": {:.2f},
    "p50_ns": {:.2f},
    "p99_ns": {:.2f}
  }}
}})",
                        stats.name, time_str, stats.count, stats.avg_ns,
                        stats.min_ns, stats.max_ns, stats.p50_ns, stats.p99_ns);

    std::println("Stats JSON exported to: {}", path.string());
  }

  // =========================================================
  // 导出分布：CSV (导出直方图 buckets)
  // =========================================================
  // 导出 "Value(ns), Count" 的分布数据。
  void export_samples_to_csv(const std::string &output_dir = "outputs") const {
    ensure_directory(output_dir);
    double ns_per_cycle = TSC::global().to_ns(1);

    std::string time_str = get_current_time_str();
    // 文件名添加 _hist 后缀以示区别
    fs::path path = fs::path(output_dir) /
                    (sanitize_filename(name_) + "_" + time_str + "_hist.csv");
    std::ofstream file(path);
    if (!file.is_open())
      return;

    file << "value_ns,count\n";
    
    // 遍历直方图并导出
    histogram_.for_each_recorded_value([&](uint64_t cycles, uint32_t count) {
       file << std::format("{:.2f},{}\n", cycles * ns_per_cycle, count);
    });

    std::println("Distribution CSV exported to: {}", path.string());
  }
  
  // 重置记录器 (清空 Histogram)
  void reset() {
    count_ = 0;
    total_cycles_ = 0.0;
    min_cycles_ = std::numeric_limits<double>::max();
    max_cycles_ = 0.0;
    histogram_.reset();
  }

private:
  struct Stats {
    std::string name;
    uint64_t count;
    double avg_ns;
    double min_ns;
    double max_ns;
    double p50_ns;
    double p99_ns;
  };

  std::string name_;
  uint64_t count_ = 0;
  double total_cycles_ = 0.0;
  double min_cycles_ = std::numeric_limits<double>::max();
  double max_cycles_ = 0.0;
  
  // 替换 std::vector<double> samples_;
  SimpleHdrHistogram histogram_;

  // 获取格式化时间串：YYYY-MM-DD-HH:MM:SS
  std::string get_current_time_str() const {
    auto now = std::chrono::system_clock::now();
    auto now_sec = std::chrono::floor<std::chrono::seconds>(now);
    return std::format("{:%Y-%m-%d-%H:%M:%S}", now_sec);
  }

  // 内部辅助：计算统计值并转为纳秒
  Stats compute_stats_ns() const {
    if (count_ == 0) {
      return {name_, 0, 0.0, 0.0, 0.0, 0.0, 0.0};
    }

    double ns_per_cycle = TSC::global().to_ns(1);

    double avg_cyc = total_cycles_ / count_;
    
    // 从 Histogram 获取 Cycles 统计值
    // 注意：Histogram 存储的是整数 Cycles
    double p50_cyc = static_cast<double>(histogram_.get_value_at_percentile(50.0));
    double p99_cyc = static_cast<double>(histogram_.get_value_at_percentile(99.0));

    return Stats{name_,
                 count_,
                 avg_cyc * ns_per_cycle,
                 min_cycles_ * ns_per_cycle,
                 max_cycles_ * ns_per_cycle,
                 p50_cyc * ns_per_cycle,
                 p99_cyc * ns_per_cycle};
  }

  void ensure_directory(const std::string &path) const {
    if (!fs::exists(path)) {
      fs::create_directories(path);
    }
  }

  std::string sanitize_filename(std::string name) const {
    std::replace_if(
        name.begin(), name.end(),
        [](char c) {
          return c == '/' || c == '\\' || c == ':' || c == ' ' || c == '<' ||
                 c == '>';
        },
        '_');
    return name;
  }
};

} // namespace eph::benchmark
