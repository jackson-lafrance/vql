#ifndef DISTINCT_OPERATOR_H
#define DISTINCT_OPERATOR_H

#include "buffer_pool.h"
#include "operator.h"
#include "operators/hash_distinct_operator.h"
#include "operators/sort_distinct_operator.h"

#include <cstddef>
#include <memory>
#include <optional>

namespace vql::algebra {
class DistinctOperator : public Operator {
public:
  enum class Mode { HASH, SORT };

  struct Options {
    std::size_t partition_count{0};  // HASH
    std::size_t workspace_frames{0}; // sORT
  };

  DistinctOperator(std::unique_ptr<Operator> input,
                   storage::BufferPool &buffer_pool, Mode mode,
                   Options options);

  const Schema &schema() const override;
  void open() override;
  void close() override;
  std::optional<Tuple> next() override;
  Counter counters() const override;

private:
  std::unique_ptr<Operator> implementation_;
};
} // namespace vql::algebra

#endif // DISTINCT_OPERATOR_H
