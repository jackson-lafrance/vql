#ifndef VQL_STORAGE_CATALOG_H
#define VQL_STORAGE_CATALOG_H

#include "buffer_pool.h"
#include "heap_file.h"
#include "schema.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vql::storage {

struct TableInfo {
  std::string name;
  algebra::Schema schema;
  std::string data_file;
  std::size_t first_page;
  std::size_t num_pages;
  std::size_t num_tuples;
};

class Catalog {
public:
  Catalog(BufferPool &buffer_pool, const std::filesystem::path &directory);

  Catalog(const Catalog &) = delete;
  Catalog &operator=(const Catalog &) = delete;
  Catalog(Catalog &&) = delete;
  Catalog &operator=(Catalog &&) = delete;

  void create_table(const std::string &name, const algebra::Schema &schema);
  TableInfo get_table(const std::string &name) const;
  std::vector<TableInfo> list_tables() const;

  std::size_t open_table(const std::string &name) const;
  RecordView get(const std::string &name, RecordID id) const;

  RecordID insert(const std::string &name, std::span<const char> record);
  void kill(const std::string &name, RecordID id);
  std::optional<std::vector<char>>
  get_statistics(const std::string &name) const;
  void set_statistics(const std::string &name, std::span<const char> bytes);
  void flush();

private:
  struct Entry {
    TableInfo table;
    std::uint32_t page_id;
    std::uint32_t statistics_page;
    bool statistics_valid;
  };

  void require_usable() const;
  std::optional<Entry> find(const std::string &name) const;
  Entry require_table(const std::string &name) const;
  Entry read_entry(std::uint32_t page_id) const;
  std::size_t open_table_file(const TableInfo &table) const;
  std::uint32_t write_entry(std::span<const char> payload);
  void update_counts(const Entry &entry, std::size_t pages, std::size_t tuples);
  void invalidate_statistics(const Entry &entry);
  std::vector<std::uint32_t> statistics_pages(std::uint32_t first) const;
  std::vector<char> read_statistics(std::uint32_t first) const;

  BufferPool &buffer_pool_;
  std::filesystem::path directory_;
  std::size_t file_id_;
  bool failed_{false};
};

} // namespace vql::storage

#endif // VQL_STORAGE_CATALOG_H
