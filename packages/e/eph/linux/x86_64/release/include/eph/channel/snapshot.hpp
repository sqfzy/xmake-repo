#pragma once

#include "eph/core/seq_lock.hpp"
#include "eph/core/shared_memory.hpp"
#include <chrono>
#include <memory>
#include <string>

namespace eph::snapshot {

/**
 * @brief 快照通道 (Snapshot Channel) - 命名空间文档
 *
 * 本模块提供基于 SeqLock 的"发布-订阅"语义通道，区别于 eph::ipc 的队列通道。
 *
 * **核心语义差异：**
 * - **Queue (IPC/ITC):** 消息流。Send/Receive
 * 是破坏性的（入队/出队）。保证顺序，不丢数据。
 * - **Snapshot (此处):** 状态流。Publish 是覆盖性的（Update），Fetch
 * 是非破坏性的（Observe）。 不保证顺序，只保证"最终一致性"和"实时性"。
 */

template <typename T, typename Backend> class Publisher {
public:
  explicit Publisher(Backend backend) : backend_(std::move(backend)) {}

  // 写入最新数据 (覆盖旧数据)
  void publish(const T &data) { backend_->store(data); }

  // 零拷贝写入
  template <typename F>
    requires std::is_invocable_v<F, T &>
  void publish(F &&writer) {
    backend_->write(std::forward<F>(writer));
  }

private:
  Backend backend_;
};

template <typename T, typename Backend> class Subscriber {
public:
  explicit Subscriber(Backend backend) : backend_(std::move(backend)) {}

  // 读取最新数据 (自旋直到一致)
  T fetch() { return backend_->load(); }
  void fetch(T &out) {
    backend_->read([&out](const T &d) { out = d; });
  }

  // 尝试读取 (如果正在写则失败)
  bool try_fetch(T &out) { return backend_->try_load(out); }

  // 访问器读取 (零拷贝)
  template <typename F>
    requires std::is_invocable_v<F, const T &>
  void fetch(F &&visitor) {
    backend_->read(std::forward<F>(visitor));
  }

  // 超时读取 (如果一直脏读则超时)
  template <class Rep, class Period>
  bool fetch(T &out, const std::chrono::duration<Rep, Period> &timeout) {
    auto start = std::chrono::steady_clock::now();
    while (!try_fetch(out)) {
      if (std::chrono::steady_clock::now() - start > timeout) {
        return false;
      }
      cpu_relax();
    }
    return true;
  }

private:
  Backend backend_;
};

// =============================================================================
// ITC (Inter-Thread Communication)
// =============================================================================

namespace itc {

template <typename T> using ItcBackend = std::shared_ptr<SeqLock<T>>;

template <typename T>
using Publisher = eph::snapshot::Publisher<T, ItcBackend<T>>;

template <typename T>
using Subscriber = eph::snapshot::Subscriber<T, ItcBackend<T>>;

template <typename T> auto channel() {
  ItcBackend<T> buffer;

  buffer = std::make_shared<SeqLock<T>>();

  return std::make_pair(Publisher<T>(buffer), Subscriber<T>(buffer));
}

} // namespace itc

// =============================================================================
// IPC (Inter-Process Communication)
// =============================================================================

namespace ipc {

template <typename T> using IpcBackend = SharedMemory<SeqLock<T>>;

template <typename T>
using Publisher = eph::snapshot::Publisher<T, IpcBackend<T>>;

template <typename T>
using Subscriber = eph::snapshot::Subscriber<T, IpcBackend<T>>;

template <typename T> auto channel(const std::string &name) {
  // Owner 创建
  IpcBackend<T> sender_shm(name, true, false);
  // User (Consumer) 打开
  IpcBackend<T> receiver_shm(name, false, false);

  return std::make_pair(Publisher<T>(std::move(sender_shm)),
                        Subscriber<T>(std::move(receiver_shm)));
}

} // namespace ipc

} // namespace eph::snapshot
