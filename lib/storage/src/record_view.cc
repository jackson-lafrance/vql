#include "record_view.h"

#include <utility>

namespace vql::storage {
std::span<const char> RecordView::bytes() const { return bytes_; }

RecordView::RecordView(BufferPool &pool, std::size_t file_id,
                       std::size_t page_id, std::span<const char> bytes)
    : pool_(&pool), file_id_(file_id), page_id_(page_id), bytes_(bytes) {}

RecordView::~RecordView() {
  if (pool_)
    pool_->unpin_page(file_id_, page_id_, false);
}

RecordView::RecordView(RecordView &&other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)), file_id_(other.file_id_),
      page_id_(other.page_id_), bytes_(other.bytes_) {}
} // namespace vql::storage
