#ifndef DISK_MANAGER_H
#define DISK_MANAGER_H

#include <array>
#include <cstddef>

using Page = std::array<char, 4096>;

namespace vql::storage {
class DiskManager {
public:
  void read_page(std::size_t page_id, Page &destination);
  void write_page(std::size_t page_id, const Page &bytes);
  std::size_t allocate_page();
  std::size_t num_pages();

  DiskManager(const DiskManager &dm) = delete;
  DiskManager(const DiskManager &&dm) = delete;

private:
  int fd_{-1};
  std::size_t read_counter{0};
  std::size_t write_counter{0};
};
} // namespace vql::storage

#endif // DISK_MANAGER_H
