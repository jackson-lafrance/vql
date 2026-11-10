#include "tuple.h"
#include <stdexcept>
#include <utility>

namespace vql::algebra {
Tuple::Tuple(std::vector<Value> values) : values_(std::move(values)) {}

std::size_t Tuple::size() const { return values_.size(); }

Value Tuple::operator[](std::size_t i) const { return values_.at(i); }

std::size_t TupleHash::operator()(const Tuple &tuple) const {
  std::size_t tuple_hash = tuple.size();
  for (std::size_t i{0}; i < tuple.size(); ++i) {
    const Value value = tuple[i];
    const std::size_t value_hash = std::hash<Value>{}(value);
    tuple_hash ^=
        value_hash + 0x9e3779b9 + (tuple_hash << 6) + (tuple_hash >> 2);
  }

  return tuple_hash;
}

void validate_tuple(const Tuple &tuple, const Schema &schema) {
  if (schema.size() != tuple.size())
    throw std::runtime_error("Tuple size of " + std::to_string(tuple.size()) +
                             " does not match Schema size of " +
                             std::to_string(schema.size()));

  for (std::size_t i{0}; i < schema.size(); ++i) {
    const Value value = tuple[i];
    if (schema[i].type.data_type() == DataType::INT &&
        std::holds_alternative<std::string>(value))
      throw std::runtime_error("Invalid tuple at position " +
                               std::to_string(i) +
                               "! Cannot match VARCHAR to INT");

    if (schema[i].type.data_type() == DataType::VARCHAR &&
        std::holds_alternative<std::int32_t>(value))
      throw std::runtime_error("Invalid tuple at position " +
                               std::to_string(i) +
                               "! Cannot match INT to VARCHAR");

    if (const std::string *str = std::get_if<std::string>(&value))
      if (str->length() > schema[i].type.max_length())
        throw std::runtime_error("Invalid tuple at position " +
                                 std::to_string(i) + "! VARCHAR of size " +
                                 std::to_string(str->length()) +
                                 " exceeds max size of " +
                                 std::to_string(schema[i].type.max_length()));
  }
}

Tuple project_tuple(const Tuple &tuple,
                    const std::vector<std::size_t> &positions) {
  std::vector<Value> values;
  values.reserve(positions.size());

  for (std::size_t position : positions)
    values.push_back(tuple[position]);

  return Tuple(std::move(values));
}

Tuple concat_tuples(const Tuple &left, const Tuple &right) {
  std::vector<Value> values;
  values.reserve(left.size() + right.size());

  for (std::size_t i{0}; i < left.size(); ++i)
    values.push_back(left[i]);
  for (std::size_t i{0}; i < right.size(); ++i)
    values.push_back(right[i]);

  return Tuple(std::move(values));
}
} // namespace vql::algebra
