#ifndef VQL_QUERY_EXECUTOR_H
#define VQL_QUERY_EXECUTOR_H

#include "catalog.h"
#include "operator.h"

#include <json/json.h>

#include <functional>
#include <string>
#include <vector>

namespace vql::query {

struct OperatorTrace {
  std::size_t depth;
  std::string label;
  algebra::Operator::Counter counters;
};

struct ExecutionTrace {
  algebra::Operator::Counter totals;
  std::vector<OperatorTrace> nodes;
};

class Executor {
public:
  explicit Executor(storage::BufferPool &pool, const storage::Catalog &catalog);

  ExecutionTrace
  run(const Json::Value &plan,
      const std::function<void(const algebra::Schema &)> &on_schema = {},
      const std::function<void(const algebra::Tuple &)> &on_row = {}) const;

private:
  storage::BufferPool &pool_;
  const storage::Catalog &catalog_;
};

} // namespace vql::query

#endif // VQL_QUERY_EXECUTOR_H
