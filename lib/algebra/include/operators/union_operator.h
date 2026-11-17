#ifndef UNION_OPERATOR_H
#define UNION_OPERATOR_H

#include "buffer_pool.h"
#include "operator.h"
#include "operators/distinct_operator.h"

#include <memory>
#include <optional>

namespace vql::algebra {
class UnionOperator : public Operator {
public:
  using Mode = DistinctOperator::Mode;
  using Options = DistinctOperator::Options;

  UnionOperator(std::unique_ptr<Operator> left_input,
                std::unique_ptr<Operator> right_input,
                storage::BufferPool &buffer_pool, Mode mode, Options options);

  const Schema &schema() const override;
  void open() override;
  void close() override;
  std::optional<Tuple> next() override;
  Counter counters() const override;

private:
  DistinctOperator distinct_;
};
} // namespace vql::algebra

#endif // UNION_OPERATOR_H
