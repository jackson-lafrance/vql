#ifndef BUFFER_POOL_H
#define BUFFER_POOL_H

#include "disk_manager.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vql::storage {
enum class Policy {
  LRU,
  CLOCK,
};

class BufferPool {
public:
  struct Stats {
    std::size_t hits{0};
    std::size_t misses{0};
    std::size_t evictions{0};
    std::size_t reads{0};
    std::size_t writes{0};
  };

  Page &fetch_page(std::size_t file_id, std::size_t page_id);
  void unpin_page(std::size_t file_id, std::size_t page_id, bool is_dirty);
  std::size_t new_page(std::size_t file_id);
  std::size_t open_file(const std::string &path);
  void flush_all();
  Stats stats() const;

  BufferPool(std::size_t frames, Policy policy);
  ~BufferPool();

  BufferPool(const BufferPool &dm) = delete;
  BufferPool &operator=(const BufferPool &dm) = delete;
  BufferPool(BufferPool &&dm) = delete;
  BufferPool &operator=(BufferPool &&dm) = delete;

private:
  struct Frame {
    Page data{};
    std::optional<std::size_t> file_id;
    std::optional<std::size_t> page_id;
    std::size_t pin_count{0};
    bool dirty{false};
    std::size_t sec{0};
    bool referenced{false};
  };

  std::size_t find_and_clean_unpinned_page();
  std::optional<std::size_t> check_cache_for_page(std::size_t file_id,
                                                  std::size_t page_id) const;
  void record_access(Frame &frame);
  void flush_frame(Frame &frame);

  std::vector<Frame> frames_;
  Stats stats_{};
  std::vector<std::unique_ptr<DiskManager>> disk_managers_;
  std::unordered_map<std::string, std::size_t> file_ids_;
  Policy policy_;
  std::size_t op{0};
  std::size_t clock_hand_{0};
};
} // namespace vql::storage

#endif // BUFFER_POOL_H
