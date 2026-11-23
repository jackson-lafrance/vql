#ifndef VQL_QUERY_STATISTICS_H
#define VQL_QUERY_STATISTICS_H

#include "tuple.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace vql::query {

using StatisticValue = std::variant<std::int32_t, std::string>;

struct HistogramBucket {
  StatisticValue minimum;
  StatisticValue maximum;
  std::size_t num_tuples{0};
  std::size_t ndv{0};
};

struct ColumnStatistics {
  std::size_t num_values{0};
  std::size_t ndv{0};
  std::optional<StatisticValue> minimum;
  std::optional<StatisticValue> maximum;
  std::vector<HistogramBucket> histogram;
};

struct TableStatistics {
  std::size_t num_tuples{0};
  std::size_t num_pages{0};
  std::size_t total_bytes{0};
  std::size_t bucket_count{20};
  std::vector<ColumnStatistics> columns;

  double width() const;
};

class HistogramBuilder {
public:
  HistogramBuilder(std::size_t expected_rows, std::size_t buckets = 20);

  void add(const algebra::Value &value);
  ColumnStatistics finish();

private:
  std::size_t expected_rows_;
  std::size_t rows_per_bucket_{0};
  std::size_t extra_rows_{0};
  std::size_t bucket_capacity_{0};
  bool finished_{false};
  ColumnStatistics statistics_;
};

} // namespace vql::query

#endif // VQL_QUERY_STATISTICS_H
