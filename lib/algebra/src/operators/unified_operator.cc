#include "operators/unified_operator.h"

#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace vql::algebra {

UnifiedOperator::UnifiedOperator(std::unique_ptr<Operator> left_input,
                                 std::unique_ptr<Operator> right_input)
    : left_(std::move(left_input)), right_(std::move(right_input)) {
  if (!left_ || !right_)
    throw std::invalid_argument("UnionAll requires two input operators!");

  const Schema &left_schema = left_->schema();
  const Schema &right_schema = right_->schema();
  if (left_schema.size() != right_schema.size())
    throw std::runtime_error("UnionAll requires schemas of the same size!");

  for (std::size_t i{0}; i < left_schema.size(); ++i) {
    if (left_schema[i].type.data_type() != right_schema[i].type.data_type())
      throw std::runtime_error("UnionAll requires matching column types!");

    if (left_schema[i].type.data_type() == DataType::VARCHAR &&
        left_schema[i].type.max_length() != right_schema[i].type.max_length())
      throw std::runtime_error("UnionAll requires matching VARCHAR sizes!");
  }

  schema_ = left_schema;
}

const Schema &UnifiedOperator::schema() const { return schema_; }

void UnifiedOperator::open() {
  if (state_ != State::CREATED)
    throw std::logic_error("UnionAll can only be opened once!");

  state_ = State::FAILED;
  for (const Schema *schema : {&left_->schema(), &right_->schema()})
    for (std::size_t i = 0; i < schema->size(); ++i)
      for (std::size_t j = 0; j < i; ++j)
        if ((*schema)[i].name == (*schema)[j].name)
          throw std::invalid_argument(
              "UnionAll input schema contains duplicate column name \"" +
              (*schema)[i].name + "\"!");

  // A failed open can still acquire resources that close must release.
  left_needs_close_ = true;
  left_->open();
  state_ = State::OPEN;
}

void UnifiedOperator::close() {
  if (state_ != State::OPEN && state_ != State::FAILED)
    throw std::logic_error(
        "UnionAll must be OPEN or FAILED before it can be CLOSED!");

  state_ = State::FAILED;
  if (left_needs_close_) {
    left_->close();
    left_needs_close_ = false;
  }
  if (right_needs_close_) {
    right_->close();
    right_needs_close_ = false;
  }

  state_ = State::CLOSED;
}

std::optional<Tuple> UnifiedOperator::next() {
  if (state_ != State::OPEN)
    throw std::logic_error("UnionAll can only be queried when open!");

  try {
    if (!trans_) {
      std::optional<Tuple> in = left_->next();
      if (in.has_value()) {
        ++counter_.tuples_out;
        return in->with_schema(schema_);
      }

      left_->close();
      left_needs_close_ = false;
      trans_ = true;
      right_needs_close_ = true;
      right_->open();
    }

    std::optional<Tuple> in = right_->next();
    if (!in.has_value())
      return std::nullopt;

    ++counter_.tuples_out;
    return in->with_schema(schema_);
  } catch (...) {
    state_ = State::FAILED;
    throw;
  }
}

Operator::Counter UnifiedOperator::counters() const { return counter_; }

} // namespace vql::algebra
