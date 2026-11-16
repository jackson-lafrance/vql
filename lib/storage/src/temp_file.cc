#include "temp_file.h"

#include "help_me_god.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace vql::storage::temp_page {
std::size_t used_end(const Page &page) {
  const std::size_t end = help_me_god::read_u16(page, 0);
  if (end < 2 || end > page.size())
    throw std::runtime_error("Invalid temporary page used-end offset!");
  return end;
}

void initialize(Page &page) {
  page.fill(0);
  help_me_god::write_u16(page, 0, 2);
}

bool append(Page &page, std::span<const char> record) {
  if (record.size() > page.size() - 4)
    throw std::length_error("Record is too large for a temporary page!");

  const std::size_t end = used_end(page);
  const std::size_t available = page.size() - end;
  if (available < 2 || record.size() > available - 2)
    return false;

  help_me_god::write_u16(page, end, static_cast<std::uint16_t>(record.size()));
  std::copy(record.begin(), record.end(), page.begin() + end + 2);
  help_me_god::write_u16(page, 0,
                         static_cast<std::uint16_t>(end + 2 + record.size()));
  return true;
}

std::optional<std::span<const char>> next(const Page &page,
                                          std::size_t &position) {
  const std::size_t end = used_end(page);
  if (position < 2 || position > end)
    throw std::out_of_range("Invalid temporary page scan position!");
  if (position == end)
    return std::nullopt;
  if (end - position < 2)
    throw std::runtime_error("Truncated temporary record length!");

  const std::size_t length = help_me_god::read_u16(page, position);
  const std::size_t start = position + 2;
  if (length > end - start)
    throw std::runtime_error("Truncated temporary record!");

  position = start + length;
  return std::span<const char>(page.data() + start, length);
}

} // namespace vql::storage::temp_page

namespace vql::storage {
TempFile::TempFile(BufferPool &buffer_pool, std::size_t file_id)
    : file_id_(file_id), buffer_pool_(buffer_pool) {}

void TempFile::append(std::span<const char> record) {
  if (record.size() > 4092)
    throw std::length_error("Record is too large for a temporary page!");

  const std::size_t page_count = buffer_pool_.num_pages(file_id_);
  if (page_count != 0) {
    const std::size_t last_page_id = page_count - 1;
    Page &page = buffer_pool_.fetch_page(file_id_, last_page_id);
    bool appended;
    try {
      appended = temp_page::append(page, record);
    } catch (...) {
      buffer_pool_.unpin_page(file_id_, last_page_id, false);
      throw;
    }
    buffer_pool_.unpin_page(file_id_, last_page_id, appended);
    if (appended)
      return;
  }

  const std::size_t page_id = buffer_pool_.new_page(file_id_);
  Page &page = buffer_pool_.fetch_page(file_id_, page_id);
  try {
    temp_page::initialize(page);
    if (!temp_page::append(page, record))
      throw std::logic_error("Record did not fit an empty temporary page!");
  } catch (...) {
    buffer_pool_.unpin_page(file_id_, page_id, true);
    throw;
  }
  buffer_pool_.unpin_page(file_id_, page_id, true);
}

TempFileReader::TempFileReader(BufferPool &buffer_pool, std::size_t file_id)
    : buffer_pool_(buffer_pool), file_id_(file_id),
      page_count_(buffer_pool.num_pages(file_id)) {}

TempFileReader::~TempFileReader() { close(); }

std::optional<std::span<const char>> TempFileReader::next() {
  if (finished_)
    return std::nullopt;

  try {
    while (page_id_ < page_count_) {
      if (page_ == nullptr)
        page_ = &buffer_pool_.fetch_page(file_id_, page_id_);

      std::optional<std::span<const char>> record =
          temp_page::next(*page_, position_);
      if (record.has_value())
        return record;

      buffer_pool_.unpin_page(file_id_, page_id_, false);
      page_ = nullptr;
      ++page_id_;
      position_ = 2;
    }

    close();
    return std::nullopt;
  } catch (...) {
    close();
    throw;
  }
}

void TempFileReader::close() {
  if (page_ != nullptr) {
    buffer_pool_.unpin_page(file_id_, page_id_, false);
    page_ = nullptr;
  }
  finished_ = true;
}
} // namespace vql::storage
