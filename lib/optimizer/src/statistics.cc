#include "statistics.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace vql::query {

StatisticValue own_statistic_value(const algebra::Value &value) {
  if (const auto *integer = std::get_if<std::int32_t>(&value))
    return *integer;
  return std::string(std::get<std::string_view>(value));
}

double TableStatistics::width() const {
  if (num_tuples == 0)
    return 0;
  return static_cast<double>(total_bytes) / static_cast<double>(num_tuples);
}

HistogramBuilder::HistogramBuilder(std::size_t expected_rows,
                                   std::size_t buckets)
    : expected_rows_(expected_rows) {
  if (buckets == 0)
    throw std::invalid_argument("Histogram bucket count cannot be zero!");
  std::size_t bucket_count = std::min(expected_rows, buckets);
  if (bucket_count != 0) {
    rows_per_bucket_ = expected_rows_ / bucket_count;
    extra_rows_ = expected_rows_ % bucket_count;
  }
}

void HistogramBuilder::add(const algebra::Value &value) {
  if (finished_)
    throw std::logic_error("Cannot add values after finishing a histogram!");
  if (statistics_.num_values >= expected_rows_)
    throw std::invalid_argument("Histogram received more rows than expected!");

  StatisticValue current = own_statistic_value(value);
  bool distinct = true;
  if (statistics_.maximum) {
    if (current.index() != statistics_.maximum->index())
      throw std::invalid_argument("Histogram values must have the same type!");
    if (current < *statistics_.maximum)
      throw std::invalid_argument("Histogram input must be sorted ascending!");
    distinct = current != *statistics_.maximum;
  }

  if (statistics_.histogram.empty() ||
      statistics_.histogram.back().num_tuples == bucket_capacity_) {
    std::size_t index = statistics_.histogram.size();
    bucket_capacity_ = rows_per_bucket_;
    if (index < extra_rows_)
      ++bucket_capacity_;
    statistics_.histogram.push_back({current, current, 1, 1});
  } else {
    HistogramBucket &bucket = statistics_.histogram.back();
    bucket.maximum = current;
    ++bucket.num_tuples;
    if (distinct)
      ++bucket.ndv;
  }

  if (distinct)
    ++statistics_.ndv;
  if (!statistics_.minimum)
    statistics_.minimum = current;
  statistics_.maximum = std::move(current);
  ++statistics_.num_values;
}

ColumnStatistics HistogramBuilder::finish() {
  if (finished_)
    throw std::logic_error("Histogram can only be finished once!");
  if (statistics_.num_values != expected_rows_)
    throw std::invalid_argument("Histogram received fewer rows than expected!");
  finished_ = true;
  return std::move(statistics_);
}

} // namespace vql::query
