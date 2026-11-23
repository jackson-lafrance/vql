#ifndef VQL_QUERY_STATISTICS_COLLECTOR_H
#define VQL_QUERY_STATISTICS_COLLECTOR_H

#include "buffer_pool.h"
#include "catalog.h"
#include "statistics.h"

namespace vql::query {

class StatisticsCollector {
public:
  StatisticsCollector(storage::BufferPool &buffer_pool,
                      storage::Catalog &catalog);

  TableStatistics analyze(const std::string &table, std::size_t buckets = 20);
  std::optional<TableStatistics> load(const std::string &table) const;

private:
  ColumnStatistics analyze_column(std::size_t snapshot,
                                  const algebra::Schema &schema,
                                  std::size_t column, std::size_t rows,
                                  std::size_t buckets) const;
  std::vector<char> encode(const TableStatistics &statistics,
                           const algebra::Schema &schema) const;
  TableStatistics decode(std::span<const char> bytes,
                         const algebra::Schema &schema) const;

  storage::BufferPool &buffer_pool_;
  storage::Catalog &catalog_;
};

} // namespace vql::query

#endif // VQL_QUERY_STATISTICS_COLLECTOR_H
