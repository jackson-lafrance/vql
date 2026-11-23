#include "statistics_collector.h"

#include "heap_scan.h"
#include "operator.h"
#include "operators/projection_operator.h"
#include "operators/sort_operator.h"
#include "temp_file.h"

#include <json/json.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace vql::query {

class StatisticsScan : public algebra::Operator {
public:
  StatisticsScan(storage::BufferPool &pool, std::size_t file,
                 algebra::Schema schema)
      : pool_(pool), file_(file) {
    schema_ = std::move(schema);
  }

  const algebra::Schema &schema() const override { return schema_; }
  Counter counters() const override { return counter_; }

  void open() override {
    if (state_ != State::CREATED)
      throw std::logic_error("Statistics scan can only be opened once!");
    state_ = State::FAILED;
    reader_ = std::make_unique<storage::TempFileReader>(pool_, file_);
    state_ = State::OPEN;
  }

  std::optional<algebra::Tuple> next() override {
    if (state_ != State::OPEN)
      throw std::logic_error("Statistics scan is not open!");
    auto before = pool_.stats();
    std::optional<algebra::Tuple> result;
    try {
      auto record = reader_->next();
      if (record)
        result.emplace(schema_, *record);
    } catch (...) {
      account_io(before);
      state_ = State::FAILED;
      throw;
    }
    account_io(before);
    if (result)
      ++counter_.tuples_out;
    return result;
  }

  void close() override {
    if (state_ != State::OPEN && state_ != State::FAILED)
      throw std::logic_error("Statistics scan cannot be closed in this state!");
    if (reader_)
      reader_->close();
    reader_.reset();
    state_ = State::CLOSED;
  }

private:
  void account_io(storage::BufferPool::Stats before) {
    auto after = pool_.stats();
    counter_.page_reads += after.reads - before.reads;
    counter_.page_writes += after.writes - before.writes;
  }

  storage::BufferPool &pool_;
  std::size_t file_;
  std::unique_ptr<storage::TempFileReader> reader_;
};

std::size_t statistics_add(std::size_t left, std::size_t right) {
  if (right > std::numeric_limits<std::size_t>::max() - left)
    throw std::overflow_error("Statistics count overflow!");
  return left + right;
}

Json::Value statistic_json(const StatisticValue &value) {
  if (const auto *integer = std::get_if<std::int32_t>(&value))
    return Json::Value(*integer);
  return Json::Value(std::get<std::string>(value));
}

std::size_t statistics_count(const Json::Value &object, const char *field) {
  const Json::Value &value = object[field];
  if ((value.type() != Json::intValue && value.type() != Json::uintValue) ||
      !value.isUInt64() ||
      value.asUInt64() > std::numeric_limits<std::size_t>::max())
    throw std::runtime_error(std::string("Invalid statistics field '") + field +
                             "'!");
  return static_cast<std::size_t>(value.asUInt64());
}

StatisticValue statistic_value(const Json::Value &value,
                               const algebra::Type &type) {
  if (type.data_type() == algebra::DataType::INT) {
    if (!value.isInt() ||
        (value.type() != Json::intValue && value.type() != Json::uintValue))
      throw std::runtime_error("Statistics value must be an INT!");
    return static_cast<std::int32_t>(value.asInt());
  }
  if (!value.isString())
    throw std::runtime_error("Statistics value must be a VARCHAR!");
  std::string text = value.asString();
  if (text.size() > type.max_length())
    throw std::runtime_error("Statistics value exceeds its VARCHAR length!");
  return text;
}

StatisticsCollector::StatisticsCollector(storage::BufferPool &buffer_pool,
                                         storage::Catalog &catalog)
    : buffer_pool_(buffer_pool), catalog_(catalog) {}

ColumnStatistics StatisticsCollector::analyze_column(
    std::size_t snapshot, const algebra::Schema &schema, std::size_t column,
    std::size_t rows, std::size_t buckets) const {
  auto input = std::make_unique<StatisticsScan>(buffer_pool_, snapshot, schema);
  auto project = std::make_unique<algebra::ProjectionOperator>(
      std::move(input), std::vector<std::string>{schema[column].name});
  algebra::SortOperator sort(std::move(project), buffer_pool_,
                             std::vector<algebra::SortKey>{{0, true}},
                             buffer_pool_.frames());
  HistogramBuilder histogram(rows, buckets);
  try {
    sort.open();
    while (auto tuple = sort.next())
      histogram.add((*tuple)[0]);
  } catch (...) {
    sort.close();
    throw;
  }
  sort.close();
  return histogram.finish();
}

TableStatistics StatisticsCollector::analyze(const std::string &table,
                                             std::size_t buckets) {
  if (buckets == 0)
    throw std::invalid_argument("ANALYZE bucket count cannot be zero!");
  if (buffer_pool_.frames() < 3)
    throw std::invalid_argument("ANALYZE requires at least three frames!");
  storage::TableInfo info = catalog_.get_table(table);
  std::size_t file = catalog_.open_table(table);
  std::size_t snapshot = buffer_pool_.create_temp_file();
  TableStatistics result;
  result.bucket_count = buckets;
  try {
    {
      storage::TempFile writer(buffer_pool_, snapshot);
      storage::HeapScan scan(buffer_pool_, file);
      while (auto record = scan.next()) {
        // Validate without copying the borrowed row before staging it.
        algebra::Tuple tuple(info.schema, record->bytes);
        writer.append(record->bytes);
        result.num_tuples = statistics_add(result.num_tuples, 1);
        result.total_bytes =
            statistics_add(result.total_bytes, record->bytes.size());
      }
      result.num_pages = scan.data_pages();
    }
    if (result.num_tuples != info.num_tuples)
      throw std::runtime_error("Table row count does not match the catalog!");
    result.columns.reserve(info.schema.size());
    for (std::size_t column{0}; column < info.schema.size(); ++column)
      result.columns.push_back(analyze_column(snapshot, info.schema, column,
                                              result.num_tuples, buckets));
  } catch (...) {
    buffer_pool_.remove_file(snapshot);
    throw;
  }
  buffer_pool_.remove_file(snapshot);

  auto current = catalog_.get_table(table);
  if (current.num_tuples != info.num_tuples ||
      current.num_pages != info.num_pages)
    throw std::runtime_error("Table changed during ANALYZE!");
  auto bytes = encode(result, info.schema);
  catalog_.set_statistics(table, bytes);
  catalog_.flush();
  return result;
}

std::optional<TableStatistics>
StatisticsCollector::load(const std::string &table) const {
  auto info = catalog_.get_table(table);
  auto bytes = catalog_.get_statistics(table);
  if (!bytes)
    return std::nullopt;
  auto statistics = decode(*bytes, info.schema);
  if (statistics.num_tuples != info.num_tuples ||
      statistics.num_pages >= info.num_pages)
    throw std::runtime_error("Stored statistics do not match the catalog!");
  return statistics;
}

std::vector<char>
StatisticsCollector::encode(const TableStatistics &statistics,
                            const algebra::Schema &schema) const {
  if (statistics.columns.size() != schema.size())
    throw std::logic_error(
        "Statistics column count does not match the schema!");
  Json::Value root(Json::objectValue);
  root["num_tuples"] = Json::UInt64(statistics.num_tuples);
  root["num_pages"] = Json::UInt64(statistics.num_pages);
  root["total_bytes"] = Json::UInt64(statistics.total_bytes);
  root["bucket_count"] = Json::UInt64(statistics.bucket_count);
  root["columns"] = Json::Value(Json::arrayValue);
  for (std::size_t i{0}; i < statistics.columns.size(); ++i) {
    const ColumnStatistics &column = statistics.columns[i];
    Json::Value node(Json::objectValue);
    node["name"] = schema[i].name;
    node["num_values"] = Json::UInt64(column.num_values);
    node["ndv"] = Json::UInt64(column.ndv);
    node["minimum"] =
        column.minimum ? statistic_json(*column.minimum) : Json::Value();
    node["maximum"] =
        column.maximum ? statistic_json(*column.maximum) : Json::Value();
    node["histogram"] = Json::Value(Json::arrayValue);
    for (const HistogramBucket &bucket : column.histogram) {
      Json::Value item(Json::objectValue);
      item["minimum"] = statistic_json(bucket.minimum);
      item["maximum"] = statistic_json(bucket.maximum);
      item["num_tuples"] = Json::UInt64(bucket.num_tuples);
      item["ndv"] = Json::UInt64(bucket.ndv);
      node["histogram"].append(std::move(item));
    }
    root["columns"].append(std::move(node));
  }
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  std::string text = Json::writeString(writer, root);
  return std::vector<char>(text.begin(), text.end());
}

TableStatistics
StatisticsCollector::decode(std::span<const char> bytes,
                            const algebra::Schema &schema) const {
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  builder["failIfExtra"] = true;
  builder["rejectDupKeys"] = true;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value root;
  std::string errors;
  if (!reader->parse(bytes.data(), bytes.data() + bytes.size(), &root, &errors))
    throw std::runtime_error("Invalid statistics JSON: " + errors);
  if (!root.isObject())
    throw std::runtime_error("Statistics must be an object!");
  TableStatistics result;
  result.num_tuples = statistics_count(root, "num_tuples");
  result.num_pages = statistics_count(root, "num_pages");
  result.total_bytes = statistics_count(root, "total_bytes");
  result.bucket_count = statistics_count(root, "bucket_count");
  if (result.bucket_count == 0 ||
      (result.num_tuples == 0 && result.total_bytes != 0) ||
      (result.num_tuples != 0 &&
       (result.total_bytes == 0 || result.num_pages == 0)))
    throw std::runtime_error("Invalid table statistics counts!");
  if (result.num_pages <= std::numeric_limits<std::size_t>::max() / 4084 &&
      result.total_bytes > result.num_pages * 4084)
    throw std::runtime_error(
        "Statistics record bytes exceed data-page capacity!");
  const Json::Value &columns = root["columns"];
  if (!columns.isArray() || columns.size() != schema.size())
    throw std::runtime_error("Statistics columns do not match the schema!");
  result.columns.reserve(schema.size());
  for (std::size_t i{0}; i < schema.size(); ++i) {
    const Json::Value &node = columns[static_cast<Json::ArrayIndex>(i)];
    if (!node.isObject() || !node["name"].isString() ||
        node["name"].asString() != schema[i].name ||
        !node.isMember("minimum") || !node.isMember("maximum"))
      throw std::runtime_error("Invalid column statistics identity!");
    ColumnStatistics column;
    column.num_values = statistics_count(node, "num_values");
    column.ndv = statistics_count(node, "ndv");
    if (column.num_values != result.num_tuples ||
        column.ndv > column.num_values ||
        (column.num_values == 0) != (column.ndv == 0))
      throw std::runtime_error("Invalid column statistics counts!");
    if (column.num_values == 0) {
      if (!node["minimum"].isNull() || !node["maximum"].isNull())
        throw std::runtime_error("Empty column has statistics bounds!");
    } else {
      column.minimum = statistic_value(node["minimum"], schema[i].type);
      column.maximum = statistic_value(node["maximum"], schema[i].type);
      if (*column.maximum < *column.minimum)
        throw std::runtime_error("Column statistics bounds are reversed!");
    }
    const Json::Value &histogram = node["histogram"];
    std::size_t count = std::min(column.num_values, result.bucket_count);
    if (!histogram.isArray() || histogram.size() != count)
      throw std::runtime_error("Invalid histogram bucket count!");
    column.histogram.reserve(count);
    std::size_t rows = 0;
    std::size_t distinct = 0;
    for (std::size_t bucket_index{0}; bucket_index < count; ++bucket_index) {
      const Json::Value &item =
          histogram[static_cast<Json::ArrayIndex>(bucket_index)];
      if (!item.isObject())
        throw std::runtime_error("Histogram bucket must be an object!");
      HistogramBucket bucket{statistic_value(item["minimum"], schema[i].type),
                             statistic_value(item["maximum"], schema[i].type),
                             statistics_count(item, "num_tuples"),
                             statistics_count(item, "ndv")};
      std::size_t expected = column.num_values / count;
      if (bucket_index < column.num_values % count)
        ++expected;
      if (bucket.num_tuples != expected || bucket.ndv == 0 ||
          bucket.ndv > bucket.num_tuples || bucket.maximum < bucket.minimum ||
          (bucket.minimum == bucket.maximum && bucket.ndv != 1) ||
          (bucket.minimum != bucket.maximum && bucket.ndv < 2))
        throw std::runtime_error("Invalid histogram bucket statistics!");
      if (!column.histogram.empty()) {
        const auto &previous = column.histogram.back();
        if (bucket.minimum < previous.maximum)
          throw std::runtime_error("Histogram ranges are out of order!");
        if (bucket.minimum == previous.maximum)
          --distinct;
      }
      rows = statistics_add(rows, bucket.num_tuples);
      distinct = statistics_add(distinct, bucket.ndv);
      column.histogram.push_back(std::move(bucket));
    }
    if (rows != column.num_values || distinct != column.ndv ||
        (count != 0 && (column.histogram.front().minimum != *column.minimum ||
                        column.histogram.back().maximum != *column.maximum)))
      throw std::runtime_error(
          "Histogram totals do not match column statistics!");
    result.columns.push_back(std::move(column));
  }
  return result;
}

} // namespace vql::query
