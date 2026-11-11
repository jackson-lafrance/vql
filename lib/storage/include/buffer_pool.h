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
      ++stats_.hits;
      frames_[cache_location.value()].pin_count++;
      if (policy_ == Policy::LRU)
        frames_[cache_location.value()].sec = ++op;
      return frames_[cache_location.value()].data;
    }

    // sad disk read :(
    std::size_t location = find_and_clean_unpinned_page();
    bool replacing = frames_[location].page_id.has_value();

    Page &page = set_frame(page_id, location);

    if (replacing)
      ++stats_.evictions;

    ++stats_.misses;
    ++stats_.reads;
    return page;
  };

  void unpin_page(std::size_t page_id, bool is_dirty) {
    std::optional<std::size_t> cache_location = check_cache_for_page(page_id);

    if (!cache_location.has_value())
      throw std::runtime_error("Page to unpin is not in cache!");

    Frame &frame = frames_[cache_location.value()];
    if (frame.pin_count == 0)
      throw std::runtime_error("Cannot unpin an already unpinned page!");

    frame.pin_count--;
    frame.dirty = frame.dirty || is_dirty;
  };

  std::size_t new_page() {
    std::size_t location = find_and_clean_unpinned_page();

    std::size_t id = disk_manager_.allocate_page();

    Frame &frame = frames_[location];
    frame.data.fill(0);
    frame.page_id = id;
    if (policy_ == Policy::LRU)
      frame.sec = ++op;
    frame.pin_count = 0;
    frame.dirty = true;

    return id;
  };

  void flush_all() {
    for (Frame &frame : frames_) {
      if (!frame.page_id.has_value() || !frame.dirty)
        continue;

      disk_manager_.write_page(frame.page_id.value(), frame.data);
      frame.dirty = false;
      ++stats_.writes;
    };
  }

  Stats stats() const { return stats_; };

  BufferPool(std::size_t frames, Policy policy, const std::string &path)
      : frames_(frames), disk_manager_(path), policy_(policy) {};
  ~BufferPool() = default;

  BufferPool(const BufferPool &dm) = delete;
  BufferPool &operator=(const BufferPool &dm) = delete;
  BufferPool(BufferPool &&dm) = delete;
  BufferPool &operator=(BufferPool &&dm) = delete;

private:
  Page &set_frame(std::size_t page_id, std::size_t frame_location) {
    if (frame_location >= frames_.size())
      throw std::logic_error("Frame location out of bounds!");

    Frame &frame = frames_[frame_location];

    frame.page_id.reset();
    frame.pin_count = 0;
    frame.dirty = false;

    disk_manager_.read_page(page_id, frame.data);

    frame.page_id = page_id;
    frame.pin_count = 1;
    if (policy_ == Policy::LRU)
      frame.sec = ++op;
    return frame.data;
  }

  std::size_t find_and_clean_unpinned_page() {
    for (std::size_t i{0}; i < frames_.size(); ++i)
      if (!frames_[i].page_id.has_value())
        return i;

    if (policy_ == Policy::LRU) {
      std::size_t lowest = frames_.size();
      for (std::size_t i{0}; i < frames_.size(); ++i) {
        if (frames_[i].pin_count == 0 &&
            (lowest == frames_.size() || frames_[i].sec < lowest))
          lowest = i;
      }

      if (lowest == frames_.size())
        throw std::runtime_error("No unpinned frame available!");

      if (frames_[lowest].dirty) {
        disk_manager_.write_page(frames_[lowest].page_id.value(),
                                 frames_[lowest].data);
        ++stats_.writes;
        frames_[lowest].dirty = false;
      }

      return lowest;
    } else {

    }
  }

  std::optional<std::size_t> check_cache_for_page(std::size_t page_id) const {
    for (std::size_t i{0}; i < frames_.size(); ++i)
      if (frames_[i].page_id == page_id)
        return i;

    return std::nullopt;
  }

  struct Frame {
    Page data{};
    std::optional<std::size_t> page_id;
    std::size_t pin_count{0};
    bool dirty{false};
    std::size_t sec{0};
  };

  std::vector<Frame> frames_;
  Stats stats_{};
  DiskManager disk_manager_;
  Policy policy_;
  std::size_t op = 0;
};
} // namespace vql::storage

#endif // BUFFER_POOL_H
