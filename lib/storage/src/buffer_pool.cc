#include "buffer_pool.h"

#include <stdexcept>

namespace vql::storage {
BufferPool::BufferPool(std::size_t frames, Policy policy,
                       const std::string &path)
    : frames_(frames), disk_manager_(path), policy_(policy) {
  if (frames == 0)
    throw std::invalid_argument("Frames cannot be 0!");
}

BufferPool::~BufferPool() = default;

Page &BufferPool::fetch_page(std::size_t page_id) {
  // happy cache :)
  std::optional<std::size_t> cache_location = check_cache_for_page(page_id);
  if (cache_location.has_value()) {
    Frame &frame = frames_[cache_location.value()];
    ++stats_.hits;
    frame.pin_count++;
    record_access(frame);
    return frame.data;
  }

  // sad disk read :(
  std::size_t location = find_and_clean_unpinned_page();
  if (location >= frames_.size())
    throw std::logic_error("Frame location out of bounds!");

  Frame &frame = frames_[location];
  bool replacing = frame.page_id.has_value();

  frame.page_id.reset();
  frame.pin_count = 0;
  frame.dirty = false;

  Page &page = frame.data;
  disk_manager_.read_page(page_id, page);

  frame.page_id = page_id;
  frame.pin_count = 1;
  record_access(frame);
  if (policy_ == Policy::CLOCK)
    clock_hand_ = (location + 1) % frames_.size();

  if (replacing)
    ++stats_.evictions;

  ++stats_.misses;
  ++stats_.reads;
  return page;
}

void BufferPool::unpin_page(std::size_t page_id, bool is_dirty) {
  std::optional<std::size_t> cache_location = check_cache_for_page(page_id);
  if (!cache_location.has_value())
    throw std::runtime_error("Page to unpin is not in cache!");

  Frame &frame = frames_[cache_location.value()];
  if (frame.pin_count == 0)
    throw std::runtime_error("Cannot unpin an already unpinned page!");

  frame.pin_count--;
  frame.dirty = frame.dirty || is_dirty;
}

std::size_t BufferPool::new_page() {
  std::size_t location = find_and_clean_unpinned_page();
  std::size_t id = disk_manager_.allocate_page();

  Frame &frame = frames_[location];
  frame.data.fill(0);
  frame.page_id = id;
  record_access(frame);
  if (policy_ == Policy::CLOCK)
    clock_hand_ = (location + 1) % frames_.size();
  frame.pin_count = 0;
  frame.dirty = true;

  return id;
}

void BufferPool::flush_all() {
  for (Frame &frame : frames_)
    flush_frame(frame);
}

BufferPool::Stats BufferPool::stats() const { return stats_; }

std::size_t BufferPool::find_and_clean_unpinned_page() {
  for (std::size_t i{0}; i < frames_.size(); ++i) {
    Frame &frame = frames_[i];
    if (!frame.page_id.has_value())
      return i;
  }

  if (policy_ == Policy::LRU) {
    std::size_t lowest = frames_.size();
    for (std::size_t i{0}; i < frames_.size(); ++i) {
      Frame &frame = frames_[i];
      if (frame.pin_count == 0 &&
          (lowest == frames_.size() || frame.sec < frames_[lowest].sec))
        lowest = i;
    }

    if (lowest == frames_.size())
      throw std::runtime_error("No unpinned frame available!");

    Frame &frame = frames_[lowest];
    flush_frame(frame);
    return lowest;
  }

  if (policy_ == Policy::CLOCK) {
    std::size_t aeonglass = frames_.size() * 2;
    while (aeonglass > 0) {
      aeonglass--;

      std::size_t index = clock_hand_;
      clock_hand_ = (clock_hand_ + 1) % frames_.size();
      Frame &frame = frames_[index];

      if (frame.pin_count > 0)
        continue;

      if (frame.referenced) {
        frame.referenced = false;
        continue;
      }

      flush_frame(frame);
      return index;
    }

    throw std::runtime_error("No unpinned frame available!");
  }

  throw std::logic_error("Unknown buffer pool policy!");
}

std::optional<std::size_t>
BufferPool::check_cache_for_page(std::size_t page_id) const {
  for (std::size_t i{0}; i < frames_.size(); ++i) {
    const Frame &frame = frames_[i];
    if (frame.page_id == page_id)
      return i;
  }

  return std::nullopt;
}

void BufferPool::record_access(Frame &frame) {
  if (policy_ == Policy::LRU)
    frame.sec = ++op;
  if (policy_ == Policy::CLOCK)
    frame.referenced = true;
}

void BufferPool::flush_frame(Frame &frame) {
  if (!frame.page_id.has_value() || !frame.dirty)
    return;

  disk_manager_.write_page(frame.page_id.value(), frame.data);
  ++stats_.writes;
  frame.dirty = false;
}
} // namespace vql::storage
