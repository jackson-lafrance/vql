#ifndef BUFFER_POOL_H
#define BUFFER_POOL_H

#include "disk_manager.h"
#include <cstddef>
#include <optional>
#include <stdexcept>
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

  Page &fetch_page(std::size_t page_id) {
    // happy cache :)
    std::optional<std::size_t> cache_location = check_cache_for_page(page_id);
    if (cache_location.has_value()) {
      frames_[cache_location.value()].pin_count++;
      return frames_[cache_location.value()].data;
    }

    // sad disk read :(
    std::size_t read_location = find_and_clean_unpinned_page();
    ++stats_.reads;
    return set_frame(page_id, read_location);
  };
  void unpin_page(std::size_t page_id, bool is_dirty);
  std::size_t new_page();
  void flush_all();
  Stats stats() const { return stats_; };

  BufferPool(std::size_t frames, Policy policy, const std::string &path)
      : frames_(frames), disk_manager_(path), policy_(policy) {};
  ~BufferPool();

  std::size_t read_counter() const;
  std::size_t write_counter() const;

  BufferPool(const BufferPool &dm) = delete;
  BufferPool &operator=(const BufferPool &dm) = delete;
  BufferPool(BufferPool &&dm) = delete;
  BufferPool &operator=(BufferPool &&dm) = delete;

private:
  Page &set_frame(std::size_t page_id, std::size_t frame_location) {
    if (frame_location > frames_.size())
      throw std::logic_error("Frame location out of bounds!");

    disk_manager_.read_page(page_id, frames_[frame_location].data);

    frames_[frame_location].page_id = page_id;
    frames_[frame_location].pin_count = 1;
    return frames_[frame_location].data;
  }

  std::size_t find_and_clean_unpinned_page() {
    for (std::size_t i{0}; i < frames_used_; ++i)
      if (frames_[i].pin_count == 0) {
        if (frames_[i].dirty) {
          disk_manager_.write_page(frames_[i].page_id.value(), frames_[i].data);
          ++stats_.writes;
          frames_[i].dirty = false;
        }
        return i;
      }

    throw std::runtime_error("No unpinned frame available!");
  }

  std::optional<std::size_t> check_cache_for_page(std::size_t page_id) {
    for (std::size_t i{0}; i < frames_used_; ++i)
      if (frames_[i].page_id == page_id) {
        ++stats_.hits;
        ++frames_[i].pin_count;
        return i;
      }

    return std::nullopt;
  }

  struct Frame {
    Page data{};
    std::optional<std::size_t> page_id;
    std::size_t pin_count{0};
    bool dirty{false};
  };

  std::vector<Frame> frames_;
  Stats stats_{};
  DiskManager disk_manager_;
  Policy policy_;
  std::size_t frames_used_{0};
};
} // namespace vql::storage

#endif // BUFFER_POOL_H
