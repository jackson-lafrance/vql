#include "buffer_pool.h"

#include <cerrno>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

namespace vql::storage {
BufferPool::BufferPool(std::size_t frames, Policy policy)
    : frames_(frames), policy_(policy) {
  if (frames == 0)
    throw std::invalid_argument("Frames cannot be 0!");
}

BufferPool::~BufferPool() = default;

Page &BufferPool::fetch_page(std::size_t file_id, std::size_t page_id) {
  if (!disk_managers_.contains(file_id))
    throw std::out_of_range("Invalid file ID!");

  // happy cache :)
  std::optional<std::size_t> cache_location =
      check_cache_for_page(file_id, page_id);
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
  frame.file_id.reset();
  frame.pin_count = 0;
  frame.dirty = false;

  Page &page = frame.data;
  DiskManager &disk_manager = *disk_managers_.at(file_id);
  disk_manager.read_page(page_id, page);

  frame.page_id = page_id;
  frame.file_id = file_id;
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

void BufferPool::unpin_page(std::size_t file_id, std::size_t page_id,
                            bool is_dirty) {
  std::optional<std::size_t> cache_location =
      check_cache_for_page(file_id, page_id);
  if (!cache_location.has_value())
    throw std::runtime_error("Page to unpin is not in cache!");

  Frame &frame = frames_[cache_location.value()];
  if (frame.pin_count == 0)
    throw std::runtime_error("Cannot unpin an already unpinned page!");

  frame.pin_count--;
  frame.dirty = frame.dirty || is_dirty;
}

std::size_t BufferPool::new_page(std::size_t file_id) {
  if (!disk_managers_.contains(file_id))
    throw std::out_of_range("Invalid file ID!");

  DiskManager &disk_manager = *disk_managers_.at(file_id);
  std::size_t location = find_and_clean_unpinned_page();
  std::size_t id = disk_manager.allocate_page();

  Frame &frame = frames_[location];
  frame.data.fill(0);
  frame.page_id = id;
  frame.file_id = file_id;
  record_access(frame);
  if (policy_ == Policy::CLOCK)
    clock_hand_ = (location + 1) % frames_.size();
  frame.pin_count = 0;
  frame.dirty = true;

  return id;
}

std::size_t BufferPool::open_file(const std::string &path) {
  if (file_ids_.contains(path))
    return file_ids_.at(path);

  if (next_file_id_ == std::numeric_limits<std::size_t>::max())
    throw std::overflow_error("File ID space is exhausted!");

  const std::size_t file_id = next_file_id_;
  disk_managers_.emplace(file_id, std::make_unique<DiskManager>(path));
  try {
    file_ids_.emplace(path, file_id);
  } catch (...) {
    disk_managers_.erase(file_id);
    throw;
  }
  ++next_file_id_;
  return file_id;
}

std::size_t BufferPool::num_pages(std::size_t file_id) const {
  if (!disk_managers_.contains(file_id))
    throw std::out_of_range("Invalid file ID!");

  return disk_managers_.at(file_id)->num_pages();
}

void BufferPool::flush_all() {
  for (Frame &frame : frames_)
    flush_frame(frame);
}

void BufferPool::remove_file(std::size_t file_id) {
  if (!disk_managers_.contains(file_id))
    throw std::logic_error("Invalid file id to remove!");

  std::string path;
  for (const auto &guy : file_ids_) {
    if (guy.second == file_id) {
      path.assign(guy.first);
    }
  }

  if (path == "")
    throw std::logic_error("Cannot find pathname to remove!");

  for (const auto &frame : frames_)
    if (frame.file_id == file_id && frame.pin_count != 0)
      throw std::logic_error("Cannot remove a pinned file!");

  if (::unlink(path.c_str()) == -1)
    throw std::system_error(errno, std::generic_category(), "unlink");

  for (auto &frame : frames_)
    if (frame.file_id == file_id) {
      frame.file_id.reset();
      frame.page_id.reset();
      frame.pin_count = 0;
      frame.dirty = false;
      frame.sec = 0;
      frame.referenced = false;
    }

  file_ids_.erase(path);
  disk_managers_.erase(file_id);
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
BufferPool::check_cache_for_page(std::size_t file_id,
                                 std::size_t page_id) const {
  for (std::size_t i{0}; i < frames_.size(); ++i) {
    const Frame &frame = frames_[i];
    if (frame.page_id == page_id && frame.file_id == file_id)
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
  if (!frame.page_id.has_value() || !frame.file_id.has_value() || !frame.dirty)
    return;

  disk_managers_.at(frame.file_id.value())
      ->write_page(frame.page_id.value(), frame.data);
  ++stats_.writes;
  frame.dirty = false;
}
} // namespace vql::storage
