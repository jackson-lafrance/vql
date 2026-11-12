#ifndef HEAP_FILE_H
#define HEAP_FILE_H

#include "buffer_pool.h"
#include "record_view.h"

#include <cstddef>
#include <span>

namespace vql::storage {
struct RecordID {
  std::size_t page_id;
  std::size_t slot_id;
};

class HeapFile {
public:
  HeapFile(BufferPool &buffer_pool, std::size_t file_id)
      : file_id_(file_id), buffer_pool_(buffer_pool) {}

  RecordID insert(std::span<const char> record);
  RecordView get(RecordID record_id);
  void kill(RecordID record_id);

private:
  std::size_t file_id_;
  BufferPool &buffer_pool_;
};
} // namespace vql::storage

#endif // HEAP_FILE_H
