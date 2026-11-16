#ifndef LIMIT_OPERATOR_H
#define LIMIT_OPERATOR_H

#include "operator.h"

#include <memory>

namespace vql::algebra {
class LimitOperator : public Operator {
public:
  LimitOperator(std::unique_ptr<Operator> input, std::size_t limit);

  const Schema &schema() const override;
  void open() override;
  void close() override;
  std::optional<Tuple> next() override;
  Counter counters() const override;

private:
  std::unique_ptr<Operator> input_;
  std::size_t limit_;
  bool sleepy_{false};
};
} // namespace vql::algebra

#endif // LIMIT_OPERATOR_H
