#ifndef RECORD_VIEW_H
#define RECORD_VIEW_H

#include "buffer_pool.h"

#include <cstddef>
#include <span>

namespace vql::storage {
class RecordView {
public:
  std::span<const char> bytes() const;

  RecordView(BufferPool &pool, std::size_t file_id, std::size_t page_id,
             std::span<const char> bytes);
  ~RecordView();

  RecordView(const RecordView &rv) = delete;
  RecordView &operator=(const RecordView &rv) = delete;

  RecordView(RecordView &&other) noexcept;

private:
  BufferPool *pool_;
  std::size_t file_id_;
  std::size_t page_id_;
  std::span<const char> bytes_;
};
} // namespace vql::storage

#endif // RECORD_VIEW_H
