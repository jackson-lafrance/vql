#include "heap_scan.h"

namespace vql::storage {
HeapScan::HeapScan(BufferPool &buffer_pool, std::size_t file_id)
    : buffer_pool_(buffer_pool), file_id_(file_id) {}

HeapScan::~HeapScan() { close(); }

std::optional<ScannedRecord> HeapScan::next() {
  // an important invariant :glasses_emoji:
  // the scanned record this guy returns is only valid until the next next()
  // call next next haha
  if (finished_)
    return std::nullopt;

  try {
    for (;;) {
      if (directory_page_ == nullptr) {
        directory_page_ =
            &buffer_pool_.fetch_page(file_id_, directory_page_id_);
      }

      if (entry_index_ >=
          help_me_god::directory_entry_count(*directory_page_)) {
        std::size_t next_id = help_me_god::next_directory(*directory_page_);

        buffer_pool_.unpin_page(file_id_, directory_page_id_, false);
        directory_page_ = nullptr;

        if (next_id == 0xFFFFFFFF) {
          close();
          return std::nullopt;
        }

        directory_page_id_ = next_id;
        entry_index_ = 0;
        slot_index_ = 0;
        continue;
      }

      if (data_page_ == nullptr) {
        data_page_id =
            help_me_god::get_page_id(*directory_page_, entry_index_);

        data_page_ = &buffer_pool_.fetch_page(file_id_, data_page_id);
      }

      while (slot_index_ < help_me_god::slot_count(*data_page_)) {
        std::size_t current_slot = slot_index_;
        ++slot_index_;

        if (!help_me_god::is_live(*data_page_, current_slot)) {
          continue;
        }

        std::span<const char> bytes =
            help_me_god::get(*data_page_, current_slot);
        return ScannedRecord(RecordID(data_page_id, current_slot), bytes);
      }

      buffer_pool_.unpin_page(file_id_, data_page_id, false);
      data_page_ = nullptr;
      entry_index_++;
      slot_index_ = 0;
    }
  } catch (...) {
    close();
    throw;
  }
}

void HeapScan::close() {
  if (directory_page_ != nullptr)
    buffer_pool_.unpin_page(file_id_, directory_page_id_, false);
  directory_page_ = nullptr;

  if (data_page_ != nullptr)
    buffer_pool_.unpin_page(file_id_, data_page_id, false);
  data_page_ = nullptr;
  finished_ = true;
}
} // namespace vql::storage
