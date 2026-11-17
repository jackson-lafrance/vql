#include "operators/distinct_operator.h"

#include <stdexcept>
#include <utility>

namespace vql::algebra {
DistinctOperator::DistinctOperator(std::unique_ptr<Operator> input,
                                   storage::BufferPool &buffer_pool, Mode mode,
                                   Options options) {
  if (!input)
    throw std::invalid_argument("Distinct is missing an input operator!");

  switch (mode) {
  case Mode::HASH:
    implementation_ = std::make_unique<HashDistinctOperator>(
        std::move(input), buffer_pool, options.partition_count);
    break;
  case Mode::SORT:
    implementation_ = std::make_unique<SortDistinctOperator>(
        std::move(input), buffer_pool, options.workspace_frames);
    break;
  default:
    throw std::invalid_argument("Invalid DISTINCT mode!");
  }
}

const Schema &DistinctOperator::schema() const {
  return implementation_->schema();
}

void DistinctOperator::open() { implementation_->open(); }

void DistinctOperator::close() { implementation_->close(); }

std::optional<Tuple> DistinctOperator::next() {
  return implementation_->next();
}

Operator::Counter DistinctOperator::counters() const {
  return implementation_->counters();
}
} // namespace vql::algebra
