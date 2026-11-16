#ifndef PROJECTION_OPERATOR_H
#define PROJECTION_OPERATOR_H

#include "operator.h"

#include <memory>
#include <string>
#include <vector>

namespace vql::algebra {
class ProjectionOperator : public Operator {
public:
  ProjectionOperator(std::unique_ptr<Operator> input,
                     std::vector<std::string> column_names);

  const Schema &schema() const override;
  void open() override;
  void close() override;
  std::optional<Tuple> next() override;
  Counter counters() const override;

private:
  std::unique_ptr<Operator> input_;
  std::vector<std::string> column_names_;
  std::vector<std::size_t> positions_;
};
} // namespace vql::algebra

#endif // PROJECTION_OPERATOR_H
