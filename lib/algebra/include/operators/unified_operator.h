#ifndef UNIFIED_OPERATOR_H
#define UNIFIED_OPERATOR_H

#include "operator.h"

#include <memory>

namespace vql::algebra {
class UnifiedOperator : public Operator {
public:
  UnifiedOperator(std::unique_ptr<Operator> left_input,
                  std::unique_ptr<Operator> right_input);

  const Schema &schema() const override;
  void open() override;
  void close() override;
  std::optional<Tuple> next() override;
  Counter counters() const override;

private:
  std::unique_ptr<Operator> left_;
  std::unique_ptr<Operator> right_;
  bool trans_{false};
  bool left_needs_close_{false};
  bool right_needs_close_{false};
};
} // namespace vql::algebra

#endif // UNIFIED_OPERATOR_H
