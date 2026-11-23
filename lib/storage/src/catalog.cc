#include "catalog.h"

#include "help_me_god.h"
#include "switcher.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace vql::storage {

std::uint64_t read_u64(const Page &page, std::size_t position) {
  return std::uint64_t{help_me_god::read_u32(page, position)} |
         (std::uint64_t{help_me_god::read_u32(page, position + 4)} << 32);
}

void write_u64(Page &page, std::size_t position, std::uint64_t number) {
  help_me_god::write_u32(page, position, static_cast<std::uint32_t>(number));
  help_me_god::write_u32(page, position + 4,
                         static_cast<std::uint32_t>(number >> 32));
}

std::size_t as_size(std::uint64_t number) {
  if (number > std::numeric_limits<std::size_t>::max())
    throw std::runtime_error(
        "Catalog counter exceeds this platform's size limit!");
  return static_cast<std::size_t>(number);
}

bool is_letter(unsigned char character) {
  return (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z') || character == '_';
}

bool identifier(const std::string &name) {
  if (name.empty() || !is_letter(static_cast<unsigned char>(name[0])))
    return false;

  for (std::size_t i{1}; i < name.size(); ++i) {
    unsigned char character = static_cast<unsigned char>(name[i]);
    if (!is_letter(character) && !(character >= '0' && character <= '9'))
      return false;
  }
  return true;
}

void validate_schema(const std::string &name, const algebra::Schema &schema) {
  if (!identifier(name))
    throw std::invalid_argument("Invalid table name '" + name + "'!");
  if (schema.size() == 0)
    throw std::invalid_argument("Table '" + name +
                                "' needs at least one column!");

  std::unordered_set<std::string> names;
  for (std::size_t i{0}; i < schema.size(); ++i) {
    const auto &column = schema[i];
    if (!identifier(column.name))
      throw std::invalid_argument("Invalid column name '" + column.name +
                                  "' in table '" + name + "'!");
    if (!names.insert(column.name).second)
      throw std::invalid_argument("Duplicate column '" + column.name +
                                  "' in table '" + name + "'!");
  }
}

void append_u32(std::vector<char> &bytes, std::size_t number) {
  if (number > std::numeric_limits<std::uint32_t>::max())
    throw std::length_error("Catalog field exceeds uint32 size limit!");
  for (unsigned shift{0}; shift < 32; shift += 8)
    bytes.push_back(static_cast<char>((number >> shift) & 0xFF));
}

void append_string(std::vector<char> &bytes, const std::string &text) {
  append_u32(bytes, text.size());
  bytes.insert(bytes.end(), text.begin(), text.end());
}

std::vector<char> encode(const std::string &name,
                         const algebra::Schema &schema) {
  std::vector<char> bytes;
  append_string(bytes, name);
  append_string(bytes, name + ".db");
  append_u32(bytes, schema.size());
  for (std::size_t i{0}; i < schema.size(); ++i) {
    const auto &column = schema[i];
    append_string(bytes, column.name);
    if (column.type.data_type() == algebra::DataType::INT) {
      append_u32(bytes, 0);
      append_u32(bytes, 0);
    } else {
      append_u32(bytes, 1);
      append_u32(bytes, column.type.max_length());
    }
  }
  return bytes;
}

std::uint32_t read_u32(std::span<const char> bytes, std::size_t &position) {
  if (position > bytes.size() || bytes.size() - position < 4)
    throw std::runtime_error("Truncated catalog integer!");

  std::uint32_t number = 0;
  for (unsigned shift{0}; shift < 32; shift += 8)
    number |= std::uint32_t{static_cast<unsigned char>(bytes[position++])}
              << shift;
  return number;
}

std::string read_string(std::span<const char> bytes, std::size_t &position) {
  std::size_t size = read_u32(bytes, position);
  if (size > bytes.size() - position)
    throw std::runtime_error("Truncated catalog string!");

  std::string text(bytes.data() + position, size);
  position += size;
  return text;
}

std::uint32_t kind(const Page &page) {
  if (help_me_god::read_u32(page, 0) != 0x50435156) // VQCP
    throw std::runtime_error("Unrecognized catalog metadata page!");

  std::uint32_t value = help_me_god::read_u32(page, 4);
  if (value != 1 && value != 2 && value != 3 && value != 4)
    throw std::runtime_error("Unknown catalog page kind!");
  return value;
}

std::uint32_t read_kind(BufferPool &pool, std::size_t file_id,
                        std::size_t page_id) {
  Page &page = pool.fetch_page(file_id, page_id);
  std::uint32_t value;
  try {
    value = kind(page);
  } catch (...) {
    pool.unpin_page(file_id, page_id, false);
    throw;
  }
  pool.unpin_page(file_id, page_id, false);
  return value;
}

void require_record_page(BufferPool &pool, std::size_t file_id,
                         const TableInfo &table, RecordID id) {
  if (id.page_id == 0 || id.page_id >= table.num_pages)
    throw std::out_of_range("Invalid record page in table '" + table.name +
                            "'!");

  std::size_t directory_id = 0;
  for (;;) {
    Page &directory = pool.fetch_page(file_id, directory_id);
    bool found = false;
    std::size_t next_id;
    try {
      std::size_t count = help_me_god::directory_entry_count(directory);
      if (help_me_god::read_u32(directory, 0) != directory_id || count > 681)
        throw std::runtime_error("Invalid heap directory in table '" +
                                 table.name + "'!");
      for (std::size_t i{0}; i < count; ++i)
        if (help_me_god::get_page_id(directory, i) == id.page_id)
          found = true;
      next_id = help_me_god::next_directory(directory);
    } catch (...) {
      pool.unpin_page(file_id, directory_id, false);
      throw;
    }
    pool.unpin_page(file_id, directory_id, false);

    if (found)
      return;
    if (next_id == 0xFFFFFFFF)
      break;
    if (next_id <= directory_id || next_id >= table.num_pages)
      throw std::runtime_error("Invalid heap directory link in table '" +
                               table.name + "'!");
    directory_id = next_id;
  }
  throw std::out_of_range("Unknown record page in table '" + table.name + "'!");
}

std::uint32_t allocate(BufferPool &pool, std::size_t file_id) {
  if (pool.num_pages(file_id) >= 0xFFFFFFFF)
    throw std::length_error("Catalog page ID space is exhausted!");
  return static_cast<std::uint32_t>(pool.new_page(file_id));
}

Catalog::Catalog(BufferPool &buffer_pool,
                 const std::filesystem::path &directory)
    : buffer_pool_(buffer_pool) {
  std::filesystem::create_directories(directory);
  directory_ = std::filesystem::canonical(directory);
  file_id_ = buffer_pool_.open_file((directory_ / "catalog.db").string());

  bool initialize = buffer_pool_.num_pages(file_id_) == 0;
  if (initialize)
    allocate(buffer_pool_, file_id_);

  Page &header = buffer_pool_.fetch_page(file_id_, 0);
  try {
    if (initialize)
      help_me_god::write_u32(header, 0, 0x4C435156); // VQCL
    else if (help_me_god::read_u32(header, 0) != 0x4C435156)
      throw std::runtime_error("Invalid catalog file header!");
  } catch (...) {
    buffer_pool_.unpin_page(file_id_, 0, initialize);
    throw;
  }
  buffer_pool_.unpin_page(file_id_, 0, initialize);
}

void Catalog::require_usable() const {
  if (failed_)
    throw std::runtime_error(
        "Catalog write failed; stop database writes and inspect the database!");
}

Catalog::Entry Catalog::read_entry(std::uint32_t page_id) const {
  std::size_t pages;
  std::size_t tuples;
  std::size_t first;
  std::uint32_t statistics_page;
  std::uint32_t statistics_valid;
  Page &entry = buffer_pool_.fetch_page(file_id_, page_id);
  try {
    if (kind(entry) != 1)
      throw std::runtime_error("Expected a committed catalog table entry!");
    pages = as_size(read_u64(entry, 16));
    tuples = as_size(read_u64(entry, 24));
    first = help_me_god::read_u32(entry, 32);
    statistics_page = help_me_god::read_u32(entry, 36);
    statistics_valid = help_me_god::read_u32(entry, 40);
    if (first != 0 || pages == 0)
      throw std::runtime_error("Invalid catalog table page information!");
    if (statistics_valid > 1 ||
        (statistics_valid == 1 && statistics_page == 0xFFFFFFFF) ||
        (statistics_page != 0xFFFFFFFF &&
         (statistics_page == 0 ||
          statistics_page >= buffer_pool_.num_pages(file_id_))))
      throw std::runtime_error("Invalid catalog statistics pointer!");
  } catch (...) {
    buffer_pool_.unpin_page(file_id_, page_id, false);
    throw;
  }
  buffer_pool_.unpin_page(file_id_, page_id, false);

  std::vector<char> payload;
  std::uint32_t current = page_id;
  bool head = true;
  for (;;) {
    Page &page = buffer_pool_.fetch_page(file_id_, current);
    std::uint32_t next;
    try {
      if (kind(page) != (head ? 1U : 2U))
        throw std::runtime_error(
            "Unexpected page kind in catalog metadata chain!");
      std::size_t start = head ? 64 : 16;
      std::size_t used = help_me_god::read_u32(page, 12);
      if (used == 0 || used > page.size() - start)
        throw std::runtime_error("Invalid catalog payload size!");
      payload.insert(payload.end(), page.begin() + start,
                     page.begin() + start + used);
      next = help_me_god::read_u32(page, 8);
    } catch (...) {
      buffer_pool_.unpin_page(file_id_, current, false);
      throw;
    }
    buffer_pool_.unpin_page(file_id_, current, false);

    if (next == 0xFFFFFFFF)
      break;
    if (next <= current || next >= buffer_pool_.num_pages(file_id_))
      throw std::runtime_error("Invalid catalog continuation link!");
    current = next;
    head = false;
  }

  std::size_t position = 0;
  std::string name = read_string(payload, position);
  std::string filename = read_string(payload, position);
  std::size_t count = read_u32(payload, position);
  if (count == 0 || count > (payload.size() - position) / 13)
    throw std::runtime_error("Invalid catalog column count!");

  std::vector<algebra::Column> columns;
  columns.reserve(count);
  for (std::size_t i{0}; i < count; ++i) {
    std::string column = read_string(payload, position);
    std::uint32_t type = read_u32(payload, position);
    std::uint32_t length = read_u32(payload, position);
    if ((type == 0 && length != 0) ||
        (type == 1 && (length == 0 || length > 255)) || type > 1)
      throw std::runtime_error("Invalid catalog column type or length!");
    if (type == 0)
      columns.push_back(
          {std::move(column), algebra::Type(algebra::DataType::INT)});
    else
      columns.push_back({std::move(column),
                         algebra::Type(algebra::DataType::VARCHAR, length)});
  }
  if (position != payload.size())
    throw std::runtime_error("Unexpected bytes after catalog schema!");

  algebra::Schema schema(std::move(columns));
  validate_schema(name, schema);
  if (filename != name + ".db" || filename == "catalog.db")
    throw std::runtime_error("Invalid catalog table data filename!");
  return {{std::move(name), std::move(schema), std::move(filename), first,
           pages, tuples},
          page_id,
          statistics_page,
          statistics_valid == 1};
}

std::optional<Catalog::Entry> Catalog::find(const std::string &name) const {
  require_usable();
  std::size_t count = buffer_pool_.num_pages(file_id_);
  if (count > 0xFFFFFFFF)
    throw std::runtime_error("Too many catalog pages!");

  for (std::size_t id{1}; id < count; ++id) {
    if (read_kind(buffer_pool_, file_id_, id) != 1)
      continue;
    auto entry = read_entry(static_cast<std::uint32_t>(id));
    if (entry.table.name == name)
      return entry;
  }
  return std::nullopt;
}

Catalog::Entry Catalog::require_table(const std::string &name) const {
  auto entry = find(name);
  if (!entry)
    throw std::out_of_range("Unknown table '" + name + "'!");
  return std::move(*entry);
}

TableInfo Catalog::get_table(const std::string &name) const {
  return require_table(name).table;
}

std::vector<TableInfo> Catalog::list_tables() const {
  require_usable();
  std::vector<TableInfo> tables;
  std::unordered_set<std::string> names;
  std::size_t count = buffer_pool_.num_pages(file_id_);
  if (count > 0xFFFFFFFF)
    throw std::runtime_error("Too many catalog pages!");

  for (std::size_t id{1}; id < count; ++id) {
    if (read_kind(buffer_pool_, file_id_, id) != 1)
      continue;
    auto entry = read_entry(static_cast<std::uint32_t>(id));
    if (!names.insert(entry.table.name).second)
      throw std::runtime_error("Duplicate catalog table name '" +
                               entry.table.name + "'!");
    tables.push_back(std::move(entry.table));
  }
  return tables;
}

std::uint32_t Catalog::write_entry(std::span<const char> payload) {
  std::uint32_t first = allocate(buffer_pool_, file_id_);
  std::uint32_t current = first;
  std::size_t position = 0;
  bool head = true;
  while (position < payload.size()) {
    std::size_t start = head ? 64 : 16;
    std::size_t used = std::min(payload.size() - position, 4096 - start);
    std::uint32_t next = 0xFFFFFFFF;
    if (used < payload.size() - position)
      next = allocate(buffer_pool_, file_id_);

    Page &page = buffer_pool_.fetch_page(file_id_, current);
    try {
      help_me_god::write_u32(page, 0, 0x50435156); // VQCP
      help_me_god::write_u32(page, 4, head ? 3 : 2);
      help_me_god::write_u32(page, 8, next);
      help_me_god::write_u32(page, 12, static_cast<std::uint32_t>(used));
      if (head) {
        write_u64(page, 16, 1);
        write_u64(page, 24, 0);
        help_me_god::write_u32(page, 32, 0);
        help_me_god::write_u32(page, 36, 0xFFFFFFFF);
        help_me_god::write_u32(page, 40, 0);
      }
      std::copy_n(payload.begin() + position, used, page.begin() + start);
    } catch (...) {
      buffer_pool_.unpin_page(file_id_, current, true);
      throw;
    }
    buffer_pool_.unpin_page(file_id_, current, true);

    position += used;
    current = next;
    head = false;
  }
  return first;
}

void Catalog::create_table(const std::string &name,
                           const algebra::Schema &schema) {
  require_usable();
  validate_schema(name, schema);
  if (find(name))
    throw std::invalid_argument("Table '" + name + "' already exists!");

  auto path = directory_ / (name + ".db");
  if (std::filesystem::exists(path))
    throw std::runtime_error("Cannot create table '" + name +
                             "': data file already exists!");
  std::vector<char> payload = encode(name, schema);
  std::size_t data_file = buffer_pool_.open_file(path.string());
  try {
    std::size_t directory_id = buffer_pool_.new_page(data_file);
    Page &directory = buffer_pool_.fetch_page(data_file, directory_id);
    try {
      help_me_god::initialize_directory(directory, 0);
    } catch (...) {
      buffer_pool_.unpin_page(data_file, directory_id, true);
      throw;
    }
    buffer_pool_.unpin_page(data_file, directory_id, true);

    std::uint32_t id = write_entry(payload);
    Page &entry = buffer_pool_.fetch_page(file_id_, id);
    try {
      help_me_god::write_u32(entry, 4, 1);
    } catch (...) {
      buffer_pool_.unpin_page(file_id_, id, true);
      throw;
    }
    buffer_pool_.unpin_page(file_id_, id, true);
  } catch (...) {
    failed_ = true;
    buffer_pool_.remove_file(data_file);
    throw;
  }
}

std::size_t Catalog::open_table_file(const TableInfo &table) const {
  auto path = directory_ / table.data_file;
  if (!std::filesystem::is_regular_file(path))
    throw std::runtime_error("Missing data file for table '" + table.name +
                             "'!");

  std::size_t file = buffer_pool_.open_file(path.string());
  if (buffer_pool_.num_pages(file) != table.num_pages)
    throw std::runtime_error("Page count mismatch for table '" + table.name +
                             "'; use catalog-managed writes!");
  return file;
}

std::size_t Catalog::open_table(const std::string &name) const {
  return open_table_file(require_table(name).table);
}

RecordView Catalog::get(const std::string &name, RecordID id) const {
  auto entry = require_table(name);
  std::size_t file = open_table_file(entry.table);
  require_record_page(buffer_pool_, file, entry.table, id);
  HeapFile heap(buffer_pool_, file);
  return heap.get(id);
}

void Catalog::update_counts(const Entry &entry, std::size_t pages,
                            std::size_t tuples) {
  Page &page = buffer_pool_.fetch_page(file_id_, entry.page_id);
  bool dirty = false;
  try {
    if (kind(page) != 1)
      throw std::runtime_error("Cannot update an uncommitted catalog entry!");
    write_u64(page, 16, pages);
    dirty = true;
    write_u64(page, 24, tuples);
  } catch (...) {
    buffer_pool_.unpin_page(file_id_, entry.page_id, dirty);
    throw;
  }
  buffer_pool_.unpin_page(file_id_, entry.page_id, true);
}

RecordID Catalog::insert(const std::string &name,
                         std::span<const char> record) {
  auto entry = require_table(name);
  if (record.size() > 4084)
    throw std::length_error("Record exceeds one heap page in table '" + name +
                            "'!");
  try {
    algebra::switcher::validate_record(record, entry.table.schema);
  } catch (const std::runtime_error &error) {
    throw std::invalid_argument("Invalid record in table '" + name +
                                "': " + error.what());
  }
  if (entry.table.num_tuples == std::numeric_limits<std::size_t>::max())
    throw std::overflow_error("Row count overflow in table '" + name + "'!");

  std::size_t file = open_table_file(entry.table);
  HeapFile heap(buffer_pool_, file);
  try {
    invalidate_statistics(entry);
    RecordID id = heap.insert(record);
    update_counts(entry, buffer_pool_.num_pages(file),
                  entry.table.num_tuples + 1);
    return id;
  } catch (...) {
    failed_ = true;
    throw;
  }
}

void Catalog::kill(const std::string &name, RecordID id) {
  auto entry = require_table(name);
  if (entry.table.num_tuples == 0)
    throw std::out_of_range("Table '" + name + "' has no live rows!");

  std::size_t file = open_table_file(entry.table);
  require_record_page(buffer_pool_, file, entry.table, id);
  HeapFile heap(buffer_pool_, file);
  {
    auto view = heap.get(id);
  }
  try {
    invalidate_statistics(entry);
    heap.kill(id);
    update_counts(entry, entry.table.num_pages, entry.table.num_tuples - 1);
  } catch (...) {
    failed_ = true;
    throw;
  }
}

void Catalog::invalidate_statistics(const Entry &entry) {
  if (!entry.statistics_valid)
    return;
  Page &page = buffer_pool_.fetch_page(file_id_, entry.page_id);
  try {
    help_me_god::write_u32(page, 40, 0);
  } catch (...) {
    buffer_pool_.unpin_page(file_id_, entry.page_id, false);
    throw;
  }
  buffer_pool_.unpin_page(file_id_, entry.page_id, true);
}

std::vector<std::uint32_t>
Catalog::statistics_pages(std::uint32_t first) const {
  std::vector<std::uint32_t> pages;
  std::uint32_t current = first;
  while (current != 0xFFFFFFFF) {
    if (current == 0 || current >= buffer_pool_.num_pages(file_id_))
      throw std::runtime_error("Invalid statistics page ID!");
    Page &page = buffer_pool_.fetch_page(file_id_, current);
    std::uint32_t next;
    try {
      if (kind(page) != 4 || help_me_god::read_u32(page, 12) > 4080)
        throw std::runtime_error("Invalid statistics page!");
      next = help_me_god::read_u32(page, 8);
      if (next != 0xFFFFFFFF && next <= current)
        throw std::runtime_error("Invalid statistics page link!");
    } catch (...) {
      buffer_pool_.unpin_page(file_id_, current, false);
      throw;
    }
    buffer_pool_.unpin_page(file_id_, current, false);
    pages.push_back(current);
    current = next;
  }
  return pages;
}

std::vector<char> Catalog::read_statistics(std::uint32_t first) const {
  std::vector<char> bytes;
  for (std::uint32_t id : statistics_pages(first)) {
    Page &page = buffer_pool_.fetch_page(file_id_, id);
    try {
      std::size_t used = help_me_god::read_u32(page, 12);
      bytes.insert(bytes.end(), page.begin() + 16, page.begin() + 16 + used);
    } catch (...) {
      buffer_pool_.unpin_page(file_id_, id, false);
      throw;
    }
    buffer_pool_.unpin_page(file_id_, id, false);
  }
  if (bytes.empty())
    throw std::runtime_error("Empty catalog statistics payload!");
  return bytes;
}

std::optional<std::vector<char>>
Catalog::get_statistics(const std::string &name) const {
  Entry entry = require_table(name);
  if (!entry.statistics_valid)
    return std::nullopt;
  return read_statistics(entry.statistics_page);
}

void Catalog::set_statistics(const std::string &name,
                             std::span<const char> bytes) {
  Entry entry = require_table(name);
  if (bytes.empty())
    throw std::invalid_argument("Statistics payload cannot be empty!");
  if (entry.statistics_valid) {
    auto previous = read_statistics(entry.statistics_page);
    if (previous.size() == bytes.size() &&
        std::equal(previous.begin(), previous.end(), bytes.begin()))
      return;
  }

  auto pages = statistics_pages(entry.statistics_page);
  std::size_t needed = bytes.size() / 4080 + (bytes.size() % 4080 != 0);
  pages.reserve(std::max(pages.size(), needed));
  try {
    invalidate_statistics(entry);
    while (pages.size() < needed) {
      std::uint32_t id = allocate(buffer_pool_, file_id_);
      Page &page = buffer_pool_.fetch_page(file_id_, id);
      try {
        help_me_god::write_u32(page, 0, 0x50435156); // VQCP
        help_me_god::write_u32(page, 4, 4);
        help_me_god::write_u32(page, 8, 0xFFFFFFFF);
        help_me_god::write_u32(page, 12, 0);
      } catch (...) {
        buffer_pool_.unpin_page(file_id_, id, true);
        throw;
      }
      buffer_pool_.unpin_page(file_id_, id, true);
      pages.push_back(id);
    }

    std::size_t position = 0;
    for (std::size_t i{0}; i < pages.size(); ++i) {
      std::size_t used = std::min(bytes.size() - position, std::size_t{4080});
      Page &page = buffer_pool_.fetch_page(file_id_, pages[i]);
      try {
        help_me_god::write_u32(
            page, 8, i + 1 < pages.size() ? pages[i + 1] : 0xFFFFFFFF);
        help_me_god::write_u32(page, 12, static_cast<std::uint32_t>(used));
        std::fill(page.begin() + 16, page.end(), 0);
        std::copy_n(bytes.begin() + position, used, page.begin() + 16);
      } catch (...) {
        buffer_pool_.unpin_page(file_id_, pages[i], true);
        throw;
      }
      buffer_pool_.unpin_page(file_id_, pages[i], true);
      position += used;
    }

    Page &table = buffer_pool_.fetch_page(file_id_, entry.page_id);
    try {
      help_me_god::write_u32(table, 36, pages[0]);
      help_me_god::write_u32(table, 40, 1);
    } catch (...) {
      buffer_pool_.unpin_page(file_id_, entry.page_id, true);
      throw;
    }
    buffer_pool_.unpin_page(file_id_, entry.page_id, true);
  } catch (...) {
    failed_ = true;
    throw;
  }
}

void Catalog::flush() {
  require_usable();
  buffer_pool_.flush_all();
}

} // namespace vql::storage
