#ifndef FILTER_OPERATOR_H
#define FILTER_OPERATOR_H

#include "operator.h"

#include <functional>
#include <memory>

namespace vql::algebra {
class FilterOperator : public Operator {
public:
  FilterOperator(std::unique_ptr<Operator> input,
                 std::function<bool(const Tuple &)> condition);

  const Schema &schema() const override;
  void open() override;
  void close() override;
  std::optional<Tuple> next() override;
  Counter counters() const override;

private:
  std::unique_ptr<Operator> input_;
  std::function<bool(const Tuple &)> condition_;
};
} // namespace vql::algebra

#endif // FILTER_OPERATOR_H
