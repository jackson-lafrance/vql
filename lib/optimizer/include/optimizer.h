#ifndef VQL_QUERY_OPTIMIZER_H
#define VQL_QUERY_OPTIMIZER_H

#include "cost_model.h"
#include "estimator.h"

#include <json/json.h>

#include <cstddef>

namespace vql::query {

class Optimizer {
public:
  Optimizer(const storage::Catalog &catalog,
            const StatisticsCollector &statistics, std::size_t workspace_frames,
            EstimatorFallbacks fallbacks = {});

  Json::Value optimize(const Json::Value &logical_plan) const;
  Json::Value dp_table(const Json::Value &logical_plan) const;

private:
  Json::Value optimize_impl(const Json::Value &logical_plan,
                            Json::Value *dp_rows) const;
  const storage::Catalog &catalog_;
  const StatisticsCollector &statistics_;
  std::size_t workspace_frames_;
  EstimatorFallbacks fallbacks_;
};

} // namespace vql::query

#endif // VQL_QUERY_OPTIMIZER_H
