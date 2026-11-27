#include "common.h"

#include <charconv>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace vql::cli {

Args::Args(int argc, char **argv) {
  if (argc < 2)
    throw std::invalid_argument("Missing command; use --help");

  command = argv[1];

  for (int i = 2; i < argc; ++i) {
    std::string key = argv[i];
    if (!key.starts_with("--"))
      throw std::invalid_argument("Expected option, got " + key);
    if (options_.count(key))
      throw std::invalid_argument("Duplicate option " + key);
    if (key == "--json" || key == "--explain" || key == "--help")
      options_[key] = "";
    else {
      if (++i == argc || std::string(argv[i]).starts_with("--"))
        throw std::invalid_argument("Missing value for " + key);
      options_[key] = argv[i];
    }
  }
}

bool Args::has(const std::string &key) const {
  return options_.count(key) != 0;
}

std::string Args::get(const std::string &key,
                      const std::string &fallback) const {
  auto it = options_.find(key);
  return it == options_.end() ? fallback : it->second;
}

std::string Args::require(const std::string &key) const {
  if (!has(key) || get(key).empty())
    throw std::invalid_argument("Required option " + key);

  return get(key);
}

std::size_t unsigned_number(const std::string &text, const std::string &label) {
  std::size_t value{};
  auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || text.empty())
    throw std::invalid_argument(label + ": expected a non-negative integer");

  return value;
}

std::size_t Args::number(const std::string &key, std::size_t fallback) const {
  return has(key) ? unsigned_number(get(key), key) : fallback;
}

void Args::allow(std::initializer_list<const char *> keys) const {
  for (const auto &[key, value] : options_) {
    bool found = key == "--help";
    for (const char *allowed : keys)
      found = found || key == allowed;
    if (!found)
      throw std::invalid_argument("Unknown option " + key + " for " + command);
  }
}

std::string read_text(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("Cannot open " + path);

  std::string text{std::istreambuf_iterator<char>(file), {}};
  if (file.bad())
    throw std::runtime_error("Cannot read " + path);

  return text;
}

Json::Value read_json(const std::string &path) {
  const std::string text = read_text(path);
  Json::CharReaderBuilder builder;
  builder["failIfExtra"] = true;
  builder["rejectDupKeys"] = true;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value value;
  std::string errors;
  if (!reader->parse(text.data(), text.data() + text.size(), &value, &errors))
    throw std::invalid_argument(path + ": " + errors);

  return value;
}

std::string json(const Json::Value &value) {
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  return Json::writeString(writer, value);
}

Json::Value schema_json(const algebra::Schema &schema) {
  Json::Value result(Json::arrayValue);
  for (std::size_t i = 0; i < schema.size(); ++i) {
    Json::Value column(Json::arrayValue);
    column.append(schema[i].name);
    const auto &type = schema[i].type;
    column.append(type.data_type() == algebra::DataType::INT ? "INT"
                                                             : "VARCHAR");
    if (type.data_type() == algebra::DataType::VARCHAR)
      column.append(Json::UInt64(type.max_length()));
    result.append(column);
  }
  return result;
}

Json::Value row_json(const algebra::Tuple &tuple) {
  Json::Value row(Json::arrayValue);
  for (std::size_t i = 0; i < tuple.size(); ++i) {
    const auto value = tuple[i];
    if (const auto *number = std::get_if<std::int32_t>(&value))
      row.append(*number);
    else
      row.append(std::string(std::get<std::string_view>(value)));
  }
  return row;
}

Json::Value stats_json(const storage::BufferPool::Stats &stats) {
  Json::Value result(Json::objectValue);
  result["hits"] = Json::UInt64(stats.hits);
  result["misses"] = Json::UInt64(stats.misses);
  result["evictions"] = Json::UInt64(stats.evictions);
  result["reads"] = Json::UInt64(stats.reads);
  result["writes"] = Json::UInt64(stats.writes);
  return result;
}

Database::Database(const Args &args)
    : pool(args.number("--frames", 32), storage::Policy::LRU),
      catalog(pool, args.has("--db") ? args.require("--db")
                                     : args.require("--catalog")) {
  if (args.has("--db") && args.has("--catalog"))
    throw std::invalid_argument("Use --db or --catalog, not both");
}

int guarded(const std::function<void()> &run) {
  try {
    run();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}

} // namespace vql::cli
