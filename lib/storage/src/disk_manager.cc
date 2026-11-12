#include "disk_manager.h"

#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace vql::storage {
namespace {
void throw_unless_interrupted(const char *operation) {
  const int error = errno;
  if (error != EINTR)
    throw std::system_error(error, std::generic_category(), operation);
}
} // namespace

DiskManager::DiskManager(const std::string &path) {
  while ((fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0600)) == -1)
    throw_unless_interrupted("open");
}

DiskManager::~DiskManager() {
  if (fd_ != -1)
    ::close(fd_);
}

void DiskManager::read_page(std::size_t page_id, Page &destination) {
  if (num_pages() <= page_id)
    throw std::logic_error("Cannot query page_id larger than file!");

  std::size_t bytes_read = 0;
  while (bytes_read < destination.size()) {
    ssize_t result =
        ::pread(fd_, destination.data() + bytes_read,
                destination.size() - bytes_read, 4096 * page_id + bytes_read);

    if (result == -1) {
      throw_unless_interrupted("pread");
      continue;
    }

    if (result == 0)
      throw std::runtime_error("Unexpected end of file while reading page!");

    bytes_read += static_cast<std::size_t>(result);
  }

  ++read_counter_;
}

void DiskManager::write_page(std::size_t page_id, const Page &source) {
  if (num_pages() <= page_id)
    throw std::logic_error("Cannot write page_id larger than file!");

  std::size_t bytes_wrote = 0;
  while (bytes_wrote < source.size()) {
    ssize_t result =
        ::pwrite(fd_, source.data() + bytes_wrote, source.size() - bytes_wrote,
                 4096 * page_id + bytes_wrote);

    if (result == -1) {
      throw_unless_interrupted("pwrite");
      continue;
    }

    if (result == 0)
      throw std::runtime_error("No progress while writing page!");

    bytes_wrote += static_cast<std::size_t>(result);
  }

  ++write_counter_;
}

std::size_t DiskManager::allocate_page() {
  std::size_t old_count = num_pages();
  while (::ftruncate(fd_, (old_count + 1) * 4096) == -1)
    throw_unless_interrupted("ftruncate");

  return old_count;
}

std::size_t DiskManager::num_pages() const {
  if (fd_ == -1)
    throw std::logic_error(
        "Number of pages cannot be queried for an unknown file!");

  struct stat info{};
  if (::fstat(fd_, &info) == -1)
    throw std::system_error(errno, std::generic_category(), "fstat");

  if (info.st_size % 4096)
    throw std::logic_error("File size must be divisible by 4096!");

  return info.st_size / 4096;
}

std::size_t DiskManager::read_counter() const {
  return read_counter_; }

std::size_t DiskManager::write_counter() const {
  return write_counter_; }
} // namespace vql::storage
