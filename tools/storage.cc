#include "common.h"
#include "disk_manager.h"
#include "heap_scan.h"
#include "help_me_god.h"
#include "parser.h"

#include <bit>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <unistd.h>

namespace vql::cli::storage_tool {

constexpr const char *help =
    R"(storage create-table --db DIR --name R --schema "a:INT,b:VARCHAR(20),c:INT"
storage load --db DIR --table R --file r.csv
storage scan --db DIR --table R --frames 8 [--limit N] --json
storage layout --db DIR --table R --page 3
storage trace --db DIR --file trace.txt --frames 16 --policy lru|clock --json
)";

algebra::Schema parse_schema(const std::string &text) {
  std::string columns = text;
  for (char &c : columns)
    if (c == ':')
      c = ' ';

  auto script =
      query::Parser::parse("CREATE TABLE cli_schema (" + columns + ");");
  if (script.statements.size() != 1 ||
      !std::holds_alternative<query::CreateTable>(script.statements[0]))
    throw std::invalid_argument("Invalid --schema");

  std::vector<algebra::Column> result;
  for (const auto &column :
       std::get<query::CreateTable>(script.statements[0]).columns) {
    if (const auto *type = std::get_if<query::VarcharType>(&column.type.value))
      result.push_back(
          {column.name.name,
           algebra::Type(algebra::DataType::VARCHAR, type->length)});
    else
      result.push_back(
          {column.name.name, algebra::Type(algebra::DataType::INT)});
  }
  return algebra::Schema(std::move(result));
}

std::vector<std::string> csv_fields(const std::string &line) {
  std::vector<std::string> fields;
  std::size_t i = 0;
  while (true) {
    std::string field;
    if (i < line.size() && line[i] == '"') {
      ++i;
      bool closed = false;
      while (i < line.size()) {
        if (line[i] != '"') {
          field += line[i++];
          continue;
        }
        ++i;
        if (i < line.size() && line[i] == '"') {
          field += '"';
          ++i;
        } else {
          closed = true;
          break;
        }
      }
      if (!closed || (i < line.size() && line[i] != ','))
        throw std::invalid_argument("column " +
                                    std::to_string(fields.size() + 1) +
                                    ": invalid quoted CSV field");
    } else {
      while (i < line.size() && line[i] != ',') {
        if (line[i] == '"')
          throw std::invalid_argument("column " +
                                      std::to_string(fields.size() + 1) +
                                      ": unexpected quote");
        field += line[i++];
      }
    }
    fields.push_back(std::move(field));
    if (i == line.size())
      return fields;
    ++i;
  }
}

std::vector<char> encode_row(const algebra::Schema &schema,
                             const std::vector<std::string> &fields) {
  if (fields.size() != schema.size())
    throw std::invalid_argument("CSV column count does not match schema");

  std::vector<char> record;
  for (std::size_t i = 0; i < schema.size(); ++i) {
    const auto &field = fields[i];
    const auto fail = [&](const std::string &reason) {
      throw std::invalid_argument("column '" + schema[i].name + "': " + reason);
    };
    if (schema[i].type.data_type() == algebra::DataType::INT) {
      std::int32_t value{};
      const auto [end, error] =
          std::from_chars(field.data(), field.data() + field.size(), value);
      if (error != std::errc{} || end != field.data() + field.size() ||
          field.empty())
        fail("invalid INT '" + field + "'");
      const auto bits = std::bit_cast<std::uint32_t>(value);
      for (unsigned shift = 0; shift < 32; shift += 8)
        record.push_back(static_cast<char>((bits >> shift) & 255));
    } else {
      if (field.size() > schema[i].type.max_length())
        fail("VARCHAR exceeds declared length");
      record.push_back(static_cast<char>(field.size() & 255));
      record.push_back(static_cast<char>((field.size() >> 8) & 255));
      record.insert(record.end(), field.begin(), field.end());
    }
  }
  return record;
}

void load(cli::Database &db, const cli::Args &args) {
  const auto table = args.require("--table");
  const auto info = db.catalog.get_table(table);
  const auto path = args.require("--file");

  std::ifstream file(path);
  if (!file)
    throw std::runtime_error("Cannot open " + path);

  std::string line;
  std::size_t number = 0;
  while (std::getline(file, line)) {
    ++number;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    std::vector<char> record;
    try {
      record = encode_row(info.schema, csv_fields(line));
    } catch (const std::exception &error) {
      db.catalog.flush();
      throw std::runtime_error("Table '" + table + "', line " +
                               std::to_string(number) + ": " + error.what());
    }

    try {
      db.catalog.insert(table, record);
    } catch (const std::exception &error) {
      throw std::runtime_error("Table '" + table + "', line " +
                               std::to_string(number) + ": " + error.what());
    }
  }
  if (file.bad())
    throw std::runtime_error("Cannot read " + path);
  db.catalog.flush();
}

void scan(cli::Database &db, const cli::Args &args) {
  const auto table = args.require("--table");
  const auto info = db.catalog.get_table(table);
  storage::HeapScan scan(db.pool, db.catalog.open_table(table));
  const bool json = args.has("--json");

  if (json)
    std::cout << "{\"table\":" << cli::json(table)
              << ",\"schema\":" << cli::json(cli::schema_json(info.schema))
              << ",\"rows\":[";

  const auto limit =
      args.number("--limit", std::numeric_limits<std::size_t>::max());
  std::size_t count = 0;
  while (count < limit) {
    const auto row = scan.next();
    if (!row)
      break;

    if (json && count)
      std::cout << ',';
    std::cout << cli::json(
        cli::row_json(algebra::Tuple(info.schema, row->bytes)));
    if (!json)
      std::cout << '\n';
    ++count;
  }

  scan.close();

  if (json)
    std::cout << "],\"stats\":" << cli::json(cli::stats_json(db.pool.stats()))
              << "}\n";
}

void layout(cli::Database &db, const cli::Args &args) {
  namespace page = storage::help_me_god;

  const auto file = db.catalog.open_table(args.require("--table"));
  const auto id = cli::unsigned_number(args.require("--page"), "--page");

  bool directory = false;
  for (std::size_t next = 0; next != 0xffffffff;) {
    const Page &p = db.pool.fetch_page(file, next);
    const auto following = page::next_directory(p);
    db.pool.unpin_page(file, next, false);
    if (next == id) {
      directory = true;
      break;
    }
    next = following;
  }

  const Page &p = db.pool.fetch_page(file, id);
  std::cout << "page_id=" << page::read_u32(p, 0);
  if (directory) {
    std::cout << " next_dir_page=" << page::next_directory(p)
              << " entry_count=" << page::directory_entry_count(p) << '\n';
    for (std::size_t i = 0; i < page::directory_entry_count(p); ++i)
      std::cout << "entry " << i << " at byte "
                << page::directory_entry_position(i)
                << ": page_id=" << page::get_page_id(p, i)
                << " free_bytes=" << page::get_page_free_bytes(p, i) << '\n';
  } else {
    std::cout << " slot_count=" << page::slot_count(p)
              << " free_end=" << page::read_u16(p, 6)
              << " free_space=" << page::free_space(p) << '\n';
    for (std::size_t i = 0; i < page::slot_count(p); ++i) {
      const auto pos = page::slot_position(i);
      std::cout << "slot " << i << " at byte " << pos
                << ": offset=" << page::read_u16(p, pos)
                << " length=" << page::read_u16(p, pos + 2) << '\n';
    }
  }
  db.pool.unpin_page(file, id, false);
}

void trace(const cli::Args &args) {
  const auto policy = args.require("--policy");
  if (policy != "lru" && policy != "clock")
    throw std::invalid_argument("--policy must be lru or clock");

  const auto frames = args.number("--frames", 16);
  const auto path = args.require("--file");
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("Cannot open " + path);

  std::size_t pages = 0, number = 0;
  std::string line;
  auto page_id = [&](std::string text) {
    if (!text.empty() && text.back() == '\r')
      text.pop_back();
    return cli::unsigned_number(text, "trace line " + std::to_string(number));
  };
  while (std::getline(input, line)) {
    ++number;
    const auto id = page_id(line);
    if (id == std::numeric_limits<std::size_t>::max())
      throw std::invalid_argument("Page id too large");
    pages = std::max(pages, id + 1);
  }
  if (input.bad())
    throw std::runtime_error("Cannot read " + path);

  const auto dir = args.require("--db");
  std::filesystem::create_directories(dir);
  std::string pattern = (std::filesystem::path(dir) / "trace-XXXXXX").string();
  const int fd = mkstemp(pattern.data());

  if (fd == -1)
    throw std::runtime_error("Cannot create trace backing file");
  close(fd);

  struct Cleanup {
    std::string path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove(path, error);
    }
  } cleanup{pattern};

  {
    storage::DiskManager disk(pattern);
    for (std::size_t i = 0; i < pages; ++i)
      disk.allocate_page();
  }

  storage::BufferPool pool(frames, policy == "lru" ? storage::Policy::LRU
                                                   : storage::Policy::CLOCK);
  const auto file = pool.open_file(pattern);
  input.clear();
  input.seekg(0);
  number = 0;
  while (std::getline(input, line)) {
    ++number;
    const auto id = page_id(line);
    pool.fetch_page(file, id);
    pool.unpin_page(file, id, false);
  }

  if (input.bad())
    throw std::runtime_error("Cannot read " + path);

  auto result = cli::stats_json(pool.stats());
  result["policy"] = policy;
  result["frames"] = Json::UInt64(frames);
  std::cout << cli::json(result) << '\n';
}

int run(int argc, char **argv) {
  return cli::guarded([&] {
    cli::Args args(argc, argv);

    if (args.command == "--help" || args.has("--help")) {
      std::cout << help;
      return;
    }

    if (args.command == "trace") {
      args.allow({"--db", "--file", "--frames", "--policy", "--json"});
      trace(args);
      return;
    }

    if (args.command == "create-table")
      args.allow({"--db", "--name", "--schema", "--frames"});
    else if (args.command == "load")
      args.allow({"--db", "--table", "--file", "--frames"});
    else if (args.command == "scan")
      args.allow({"--db", "--table", "--frames", "--limit", "--json"});
    else if (args.command == "layout")
      args.allow({"--db", "--table", "--page", "--frames"});
    else
      throw std::invalid_argument("Unknown storage command " + args.command);

    cli::Database db(args);

    if (args.command == "create-table") {
      db.catalog.create_table(args.require("--name"),
                              parse_schema(args.require("--schema")));
      db.catalog.flush();
    } else if (args.command == "load")
      load(db, args);
    else if (args.command == "scan")
      scan(db, args);
    else
      layout(db, args);
  });
}

} // namespace vql::cli::storage_tool

int main(int argc, char **argv) {
  return vql::cli::storage_tool::run(argc, argv);
}
