#include "operators/filter_operator.h"

#include <stdexcept>
#include <utility>

namespace vql::algebra {

FilterOperator::FilterOperator(std::unique_ptr<Operator> input,
                               std::function<bool(const Tuple &)> condition)
    : input_(std::move(input)), condition_(std::move(condition)) {
  if (!input_)
    throw std::invalid_argument("Filter is missing an input operator!");
  if (!condition_)
    throw std::invalid_argument("Filter is missing a condition!");

  schema_ = input_->schema();
}

const Schema &FilterOperator::schema() const { return schema_; }

void FilterOperator::open() {
  if (state_ != State::CREATED)
    throw std::logic_error("Filter can only be opened once!");

  state_ = State::FAILED;
  input_->open();
  state_ = State::OPEN;
}

std::optional<Tuple> FilterOperator::next() {
  if (state_ != State::OPEN)
    throw std::logic_error("Filter can only be queried when open!");

  for (;;) {
    std::optional<Tuple> in = input_->next();

    if (!in.has_value())
      return std::nullopt;

    ++counter_.comparisons;
    if (condition_(in.value())) {
      ++counter_.tuples_out;
      return in;
    }
  }
}

void FilterOperator::close() {
  if (state_ != State::OPEN && state_ != State::FAILED)
    throw std::logic_error(
        "Filter must be open or failed before it can be closed!");

  state_ = State::FAILED;
  input_->close();
  state_ = State::CLOSED;
}

Operator::Counter FilterOperator::counters() const { return counter_; }

} // namespace vql::algebra
