#ifndef HEAP_SCAN_H
#define HEAP_SCAN_H

#include "buffer_pool.h"
#include "heap_file.h"
#include "help_me_god.h"
#include <cstddef>
#include <optional>
#include <span>

namespace vql::storage {
struct ScannedRecord {
  RecordID id;
  std::span<const char> bytes;
};

class HeapScan {
public:
  HeapScan(BufferPool &buffer_pool, std::size_t file_id);
  ~HeapScan();
  HeapScan(const HeapScan &) = delete;
  HeapScan &operator=(const HeapScan &) = delete;

  std::optional<ScannedRecord> next();
  void close();
  std::size_t data_pages() const;

private:
  std::size_t entry_index_{0};
  std::size_t slot_index_{0};
  std::size_t data_pages_{0};
  bool finished_{false};

  BufferPool &buffer_pool_;
  std::size_t file_id_;

  Page *directory_page_{nullptr};
  Page *data_page_{nullptr};
  std::size_t directory_page_id_{0};
  std::size_t data_page_id{0};
};
} // namespace vql::storage

#endif // HEAP_SCAN_H
