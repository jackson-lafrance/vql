#ifndef DISK_MANAGER_H
#define DISK_MANAGER_H

#include <array>
#include <cstddef>
#include <string>

using Page = std::array<char, 4096>;

namespace vql::storage {
class DiskManager {
public:
  void read_page(std::size_t page_id, Page &destination);
  void write_page(std::size_t page_id, const Page &source);
  std::size_t allocate_page();
  std::size_t num_pages() const;

  explicit DiskManager(const std::string &path);
  ~DiskManager();

  std::size_t read_counter() const;
  std::size_t write_counter() const;

  DiskManager(const DiskManager &dm) = delete;
  DiskManager &operator=(const DiskManager &dm) = delete;
  DiskManager(DiskManager &&dm) = delete;
  DiskManager &operator=(DiskManager &&dm) = delete;

private:
  int fd_{-1};
  std::size_t read_counter_{0};
  std::size_t write_counter_{0};
};
} // namespace vql::storage

#endif // DISK_MANAGER_H
