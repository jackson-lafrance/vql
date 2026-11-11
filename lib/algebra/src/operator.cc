#include "operator.h"

#include <stdexcept>
#include <utility>

namespace vql::algebra {

ProjectionOperator::ProjectionOperator(std::unique_ptr<Operator> input,
                                       std::vector<std::string> column_names)
    : input_(std::move(input)), column_names_(std::move(column_names)) {
  if (!input_)
    throw std::invalid_argument("Projection is missing an input operator!");

  const Schema &input_schema = input_->schema();
  positions_.reserve(column_names_.size());
  try {
    for (const std::string &name : column_names_)
      positions_.push_back(input_schema.index_of(name));
  } catch (const std::runtime_error &error) {
    throw std::runtime_error(std::string("Projection: ") + error.what() + "!");
  }
  schema_ = project_schema(input_schema, positions_);
}

const Schema &ProjectionOperator::schema() const { return schema_; }

void ProjectionOperator::open() {
  if (state_ != State::CREATED)
    throw std::logic_error("Projection can only be opened once!");

  state_ = State::FAILED;
  input_->open();
  state_ = State::OPEN;
}

std::optional<Tuple> ProjectionOperator::next() {
  if (state_ != State::OPEN)
    throw std::logic_error("Projection can only be queried when open!");

  std::optional<Tuple> in = input_->next();

  if (!in.has_value())
    return std::nullopt;

  const Tuple &tuple = in.value();

  std::vector<Value> values;
  values.reserve(schema_.size());

  for (std::size_t position : positions_)
    values.push_back(tuple[position]);

  Tuple out(std::move(values));
  ++counter_.tuples_out;

  return out;
}

void ProjectionOperator::close() {
  if (state_ != State::OPEN && state_ != State::FAILED)
    throw std::logic_error(
        "Projection must be open or failed before it can be closed!");

  state_ = State::FAILED;
  input_->close();
  state_ = State::CLOSED;
}

Operator::Counter ProjectionOperator::counters() const { return counter_; }

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
