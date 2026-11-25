#ifndef TEMP_FILE_H
#define TEMP_FILE_H

#include "buffer_pool.h"

#include <cstddef>
#include <optional>
#include <span>

namespace vql::storage::temp_page {
std::size_t used_end(const Page &page);

void initialize(Page &page);

bool append(Page &page, std::span<const char> record);

std::optional<std::span<const char>> next(const Page &page,
                                          std::size_t &position);
} // namespace vql::storage::temp_page

namespace vql::storage {
class TempFile {
public:
  TempFile(BufferPool &buffer_pool, std::size_t file_id);

  void append(std::span<const char> record);

private:
  std::size_t file_id_;
  BufferPool &buffer_pool_;
};

class TempFileReader {
public:
  TempFileReader(BufferPool &buffer_pool, std::size_t file_id);
  ~TempFileReader();
  TempFileReader(const TempFileReader &) = delete;
  TempFileReader &operator=(const TempFileReader &) = delete;

  std::optional<std::span<const char>> next();
  void release_page();
  std::span<const char> current();
  void close();

private:
  BufferPool &buffer_pool_;
  std::size_t file_id_;
  std::size_t page_count_;
  std::size_t page_id_{0};
  std::size_t position_{2};
  std::optional<std::size_t> current_position_;
  Page *page_{nullptr};
  bool finished_{false};
};
} // namespace vql::storage

#endif // TEMP_FILE_H
