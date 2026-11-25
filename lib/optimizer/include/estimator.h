#ifndef VQL_QUERY_ESTIMATOR_H
#define VQL_QUERY_ESTIMATOR_H

#include "ast.h"
#include "catalog.h"
#include "statistics.h"
#include "statistics_collector.h"

#include <json/json.h>

#include <cstddef>
#include <optional>

namespace vql::query {

struct RelationEstimate {
  std::size_t num_tuples{0};
  std::size_t num_pages{0};
  double width{0};
};

struct ColumnStatisticsBinding {
  std::string qualifier;
  std::string column_name;
  std::size_t position{0};
  std::size_t relation_rows{0};
  double width{0};
  const ColumnStatistics *statistics{nullptr};
};

struct EstimatorFallbacks {
  double unknown_range_selectivity{0.1};
  double varchar_bucket_fraction{0.1};
  double set_overlap_fraction{0.1};
};

class CardinalityEstimator {
public:
  std::size_t distinct_values(std::size_t rows,
                              std::optional<std::size_t> observed_ndv = {}) const;

  double equality_selectivity(std::size_t rows,
                              const ColumnStatistics *statistics,
                              const StatisticValue &value) const;

  std::size_t equality_join_rows(std::size_t left_rows,
                                 const ColumnStatistics *left,
                                 std::size_t right_rows,
                                 const ColumnStatistics *right) const;

  std::size_t output_rows(std::size_t input_rows, double selectivity) const;
  double and_selectivity(double left, double right) const;
  double or_selectivity(double left, double right) const;
  double not_selectivity(double input) const;

  double condition_selectivity(
      const Condition &condition,
      const std::vector<ColumnStatisticsBinding> &columns,
      const EstimatorFallbacks &fallbacks) const;

  std::size_t output_pages(std::size_t rows, double width) const;
  RelationEstimate filter(const RelationEstimate &input,
                          double selectivity) const;
};

class JsonPlanEstimator {
public:
  JsonPlanEstimator(const storage::Catalog &catalog,
                    const StatisticsCollector &statistics,
                    EstimatorFallbacks fallbacks = {});

  Json::Value estimate(const Json::Value &logical_plan) const;

private:
  const storage::Catalog &catalog_;
  const StatisticsCollector &statistics_;
  EstimatorFallbacks fallbacks_;
};

} // namespace vql::query

#endif // VQL_QUERY_ESTIMATOR_H
