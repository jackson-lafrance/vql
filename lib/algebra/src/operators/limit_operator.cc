#include "operators/limit_operator.h"

#include <stdexcept>
#include <utility>

namespace vql::algebra {

LimitOperator::LimitOperator(std::unique_ptr<Operator> input, std::size_t limit)
    : input_(std::move(input)), limit_(limit) {
  if (!input_)
    throw std::invalid_argument("Limit is missing an input operator!");

  schema_ = input_->schema();
}

const Schema &LimitOperator::schema() const { return schema_; }

void LimitOperator::open() {
  if (state_ != State::CREATED)
    throw std::logic_error("Limit can only be opened once!");

  state_ = State::FAILED;
  input_->open();
  state_ = State::OPEN;
}

std::optional<Tuple> LimitOperator::next() {
  if (state_ != State::OPEN)
    throw std::logic_error("Limit can only be queried when open!");

  if (counter_.tuples_out >= limit_ || sleepy_)
    return std::nullopt;

  std::optional<Tuple> in = input_->next();
  if (!in.has_value()) {
    sleepy_ = true;
    return std::nullopt;
  }

  ++counter_.tuples_out;
  return in;
}

void LimitOperator::close() {
  if (state_ != State::OPEN && state_ != State::FAILED)
    throw std::logic_error(
        "Limit must be open or failed before it can be closed!");

  state_ = State::FAILED;
  input_->close();
  state_ = State::CLOSED;
}

Operator::Counter LimitOperator::counters() const { return counter_; }

} // namespace vql::algebra
