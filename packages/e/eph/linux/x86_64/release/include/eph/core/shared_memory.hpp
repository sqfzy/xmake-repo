#pragma once

#include "eph/platform.hpp"
#include "eph/types.hpp"
#include <atomic>
#include <cassert>
#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <system_error>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace eph {

using namespace eph::detail;

namespace detail {

/**
 * @brief 内部资源句柄
 */
struct RawShmHandle {
  int fd = -1;
  void *addr = nullptr;
  size_t map_size = 0;
  std::string full_path;

  bool is_valid() const { return addr != nullptr && addr != MAP_FAILED; }
};

/**
 * @brief 路径解析
 * 根据模式选择 /dev/shm 或 /dev/hugepages
 */
inline std::string resolve_path(std::string_view name, bool use_huge_pages) {
  std::string_view base_dir = use_huge_pages ? "/dev/hugepages" : "/dev/shm";

  std::filesystem::path p(std::string(base_dir) + "/" + std::string(name));
  return p.lexically_normal().string();
}

/**
 * @brief 底层内存映射逻辑
 */
inline RawShmHandle map_raw_bytes(std::string_view name, size_t size,
                                  bool is_owner, bool use_huge_pages) {
  RawShmHandle handle;
  handle.map_size = size;
  handle.full_path = resolve_path(name, use_huge_pages);

  int flags = O_RDWR;
  if (is_owner) {
    flags |= O_CREAT | O_EXCL;
    // 确保清理旧文件（无论是否正常退出），防止 EEXIST
    ::unlink(handle.full_path.c_str());
  }

  // 1. Open
  handle.fd = ::open(handle.full_path.c_str(), flags, S_IRUSR | S_IWUSR);
  if (handle.fd == -1) {
    throw std::system_error(errno, std::generic_category(),
                            "open failed: " + handle.full_path);
  }

  // 守卫 fd，防止后续异常导致泄漏
  std::unique_ptr<int, void (*)(int *)> fd_guard(&handle.fd, [](int *fd) {
    if (*fd != -1) {
      ::close(*fd);
    }
  });

  // 2. 设置/检查大小 (ftruncate/fstat)
  // 对于 hugetlbfs，ftruncate 不是必须的（因为 mmap
  // 会分配），但调用它是安全的且能设置元数据
  if (is_owner) {
    if (::ftruncate(handle.fd, handle.map_size) == -1) {
      throw std::system_error(errno, std::generic_category(),
                              "ftruncate failed");
    }
  } else {
    struct stat s;
    if (::fstat(handle.fd, &s) == -1) {
      throw std::system_error(errno, std::generic_category(), "fstat failed");
    }
    // Consumer 检查实际文件大小，防止映射空文件会导致总线错误 (SIGBUS)。
    // 例如，owner 执行 shm_open 后马上挂起了，Consumer 会成功
    // mmap 但访问共享区域时会崩溃。
    if (static_cast<size_t>(s.st_size) < handle.map_size) {
      throw std::runtime_error("Shared memory size mismatch: file too small");
    }
  }

  // 3. 内存映射 (mmap)
  int mmap_flags = MAP_SHARED;
  if (use_huge_pages) {
    mmap_flags |= MAP_HUGETLB;
  }

  handle.addr = ::mmap(nullptr, handle.map_size, PROT_READ | PROT_WRITE,
                       mmap_flags, handle.fd, 0);

  if (handle.addr == MAP_FAILED) {
    // 针对 HugePages 的特定错误提示
    if (use_huge_pages && (errno == EINVAL || errno == ENOMEM)) {
      throw std::system_error(errno, std::generic_category(),
                              "mmap failed (Huge Pages enabled: check "
                              "/proc/sys/vm/nr_hugepages)");
    }
    throw std::system_error(errno, std::generic_category(), "mmap failed");
  }

  // 释放 guard，fd 的所有权转移给 handle
  std::ignore = fd_guard.release();
  return handle;
}

/**
 * @brief 底层资源释放
 */
inline void unmap_raw_bytes(RawShmHandle &handle, bool is_owner) {
  if (handle.addr && handle.addr != MAP_FAILED) {
    ::munmap(handle.addr, handle.map_size);
    handle.addr = nullptr;
  }

  if (handle.fd != -1) {
    ::close(handle.fd);
    handle.fd = -1;
  }

  if (is_owner && !handle.full_path.empty()) {
    ::unlink(handle.full_path.c_str());
  }
}

} // namespace detail

/**
 * @brief 共享内存 (SHM) 的 RAII 封装
 *
 * 特性:
 * 1. 自动计算对齐与 Padding，避免 False Sharing
 * 2. 统一底层逻辑，直接操作 /dev/shm 或 /dev/hugepages
 * 3. 使用 C++20 atomic::wait 进行低延迟同步
 */
template <typename T>
  requires ShmLayout<T>
class SharedMemory {
private:
  struct Layout {
    alignas(CACHE_LINE_SIZE) std::atomic<bool> initialized{false};
    alignas(alignof(T) > CACHE_LINE_SIZE ? alignof(T) : CACHE_LINE_SIZE) T data;
  };

public:
  SharedMemory(std::string name, bool is_owner, bool use_huge_pages = false)
      : is_owner_(is_owner), use_huge_pages_(use_huge_pages) {

    size_t raw_size = sizeof(Layout);
    if (use_huge_pages_) {
      raw_size = align_up<HUGE_PAGE_SIZE>(raw_size);
    }

    handle_ = detail::map_raw_bytes(name, raw_size, is_owner_, use_huge_pages_);
    layout_ = static_cast<Layout *>(handle_.addr);

    initialize_layout();
  }

  ~SharedMemory() { cleanup(); }

  // Non-copyable
  SharedMemory(const SharedMemory &) = delete;
  SharedMemory &operator=(const SharedMemory &) = delete;

  // Movable
  SharedMemory(SharedMemory &&other) noexcept
      : handle_(std::move(other.handle_)), layout_(other.layout_),
        is_owner_(other.is_owner_), use_huge_pages_(other.use_huge_pages_) {
    other.layout_ = nullptr;
    other.is_owner_ = false;
    other.handle_ = {};
  }

  SharedMemory &operator=(SharedMemory &&other) noexcept {
    if (this != &other) {
      cleanup();
      handle_ = std::move(other.handle_);
      layout_ = other.layout_;
      is_owner_ = other.is_owner_;
      use_huge_pages_ = other.use_huge_pages_;

      other.layout_ = nullptr;
      other.is_owner_ = false;
      other.handle_ = {};
    }
    return *this;
  }

  T *operator->() noexcept { return data(); }
  const T *operator->() const noexcept { return data(); }
  explicit operator bool() const noexcept { return layout_ != nullptr; }

  [[nodiscard]] T *data() noexcept {
    return layout_ ? &layout_->data : nullptr;
  }
  [[nodiscard]] const T *data() const noexcept {
    return layout_ ? &layout_->data : nullptr;
  }
  [[nodiscard]] const std::string &name() const noexcept {
    return handle_.full_path; // 返回完整路径更具信息量
  }

  static SharedMemory<T> create(std::string name, bool use_huge_pages = false) {
    return SharedMemory<T>(std::move(name), true, use_huge_pages);
  }

  static SharedMemory<T> open(std::string name, bool use_huge_pages = false) {
    return SharedMemory<T>(std::move(name), false, use_huge_pages);
  }

private:
  detail::RawShmHandle handle_;
  Layout *layout_ = nullptr;
  bool is_owner_ = false;
  bool use_huge_pages_ = false;

  void initialize_layout() {
    if (is_owner_) {
      std::construct_at(&layout_->data);
      layout_->initialized.store(true, std::memory_order_release);
      layout_->initialized.notify_all();
    } else {
      // 等待初始化完成
      while (!layout_->initialized.load(std::memory_order_acquire)) {
        layout_->initialized.wait(false);
      }
    }
  }

  void cleanup() noexcept {
    if (layout_) {
      if (is_owner_) {
        if constexpr (!std::is_trivially_destructible_v<T>) {
          std::destroy_at(&layout_->data);
        }
      }
      detail::unmap_raw_bytes(handle_, is_owner_);
      layout_ = nullptr;
    }
  }
};

} // namespace eph
