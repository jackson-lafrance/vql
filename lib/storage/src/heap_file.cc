#include "heap_file.h"

#include "help_me_god.h"

#include <stdexcept>

namespace vql::storage {
HeapFile::HeapFile(BufferPool &buffer_pool, std::size_t file_id)
    : file_id_(file_id), buffer_pool_(buffer_pool) {}

RecordID HeapFile::insert(std::span<const char> record) {
  if (record.size() > 4084)
    throw std::runtime_error("Record cannot be larger than 4084 bytes!");

  std::size_t directory_id = 0;

  for (;;) {
    Page &directory = buffer_pool_.fetch_page(file_id_, directory_id);
    bool directory_dirty = false;
    std::size_t next_id;

    try {
      for (std::size_t i{0}; i < help_me_god::directory_entry_count(directory);
           ++i) {
        if (help_me_god::get_page_free_bytes(directory, i) < record.size() + 4)
          continue;

        std::size_t data_page_id = help_me_god::get_page_id(directory, i);
        Page &data = buffer_pool_.fetch_page(file_id_, data_page_id);
        bool data_dirty = false;
        std::size_t slot_id;

        try {
          slot_id = help_me_god::insert(data, record);
          data_dirty = true;
          help_me_god::update_free_bytes(directory, i,
                                         help_me_god::free_space(data));
          directory_dirty = true;
        } catch (...) {
          buffer_pool_.unpin_page(file_id_, data_page_id, data_dirty);
          throw;
        }

        buffer_pool_.unpin_page(file_id_, data_page_id, true);
        buffer_pool_.unpin_page(file_id_, directory_id, true);
        return {data_page_id, slot_id};
      }

      next_id = help_me_god::next_directory(directory);
    } catch (...) {
      buffer_pool_.unpin_page(file_id_, directory_id, directory_dirty);
      throw;
    }

    buffer_pool_.unpin_page(file_id_, directory_id, false);
    if (next_id == 0xFFFFFFFF)
      break;
    directory_id = next_id;
  }

  directory_id = 0;

  for (;;) {
    Page &directory = buffer_pool_.fetch_page(file_id_, directory_id);
    bool directory_dirty = false;
    std::size_t next_id;

    try {
      if (help_me_god::directory_entry_count(directory) < 681) {
        std::size_t new_page_id = buffer_pool_.new_page(file_id_);
        Page &new_page = buffer_pool_.fetch_page(file_id_, new_page_id);
        std::size_t slot;

        try {
          help_me_god::initialize(new_page, new_page_id);
          slot = help_me_god::insert(new_page, record);
          help_me_god::append_entry(directory, new_page_id,
                                    help_me_god::free_space(new_page));
          directory_dirty = true;
        } catch (...) {
          buffer_pool_.unpin_page(file_id_, new_page_id, true);
          throw;
        }

        buffer_pool_.unpin_page(file_id_, new_page_id, true);
        buffer_pool_.unpin_page(file_id_, directory_id, true);
        return {new_page_id, slot};
      }

      next_id = help_me_god::next_directory(directory);
      if (next_id == 0xFFFFFFFF) {
        next_id = buffer_pool_.new_page(file_id_);
        Page &new_directory = buffer_pool_.fetch_page(file_id_, next_id);
        try {
          help_me_god::initialize_directory(new_directory, next_id);
        } catch (...) {
          buffer_pool_.unpin_page(file_id_, next_id, true);
          throw;
        }
        buffer_pool_.unpin_page(file_id_, next_id, true);

        help_me_god::set_next_directory(directory, next_id);
        directory_dirty = true;
      }
    } catch (...) {
      buffer_pool_.unpin_page(file_id_, directory_id, directory_dirty);
      throw;
    }

    buffer_pool_.unpin_page(file_id_, directory_id, directory_dirty);
    directory_id = next_id;
  }
}

RecordView HeapFile::get(RecordID record_id) {
  Page &page = buffer_pool_.fetch_page(file_id_, record_id.page_id);
  try {
    std::span<const char> bytes = help_me_god::get(page, record_id.slot_id);
    return RecordView(buffer_pool_, file_id_, record_id.page_id, bytes);
  } catch (...) {
    buffer_pool_.unpin_page(file_id_, record_id.page_id, false);
    throw;
  }
}

void HeapFile::kill(RecordID record_id) {
  Page &page = buffer_pool_.fetch_page(file_id_, record_id.page_id);
  try {
    help_me_god::kill(page, record_id.slot_id);
  } catch (...) {
    buffer_pool_.unpin_page(file_id_, record_id.page_id, false);
    throw;
  }
  buffer_pool_.unpin_page(file_id_, record_id.page_id, true);
}
} // namespace vql::storage
