#include "tuple.h"

#include "switcher.h"

#include <stdexcept>
#include <string>

namespace vql::algebra {
Tuple::Tuple(const Schema &schema, std::span<const char> record)
    : schema_(&schema), record_schema_(&schema), record_(record) {
  switcher::validate_record(record, schema);
}

std::size_t Tuple::size() const { return schema_->size(); }

const Schema &Tuple::schema() const { return *schema_; }

std::size_t Tuple::physical_position(std::size_t i) const {
  if (i >= size())
    throw std::out_of_range("Tuple column position is out of range!");
  return columns_.empty() ? i : columns_[i];
}

Value Tuple::operator[](std::size_t i) const { return value_at(i); }

Value Tuple::value_at(std::size_t i) const {
  const std::size_t position = physical_position(i);
  const auto field =
      switcher::record_field_bytes(record_, *record_schema_, position);
  if ((*record_schema_)[position].type.data_type() == DataType::INT)
    return switcher::record_int(field);
  return std::string_view(field.data(), field.size());
}

std::optional<std::span<const char>> Tuple::encoded_record() const {
  if (size() != record_schema_->size())
    return std::nullopt;
  for (std::size_t i = 0; i < columns_.size(); ++i)
    if (columns_[i] != i)
      return std::nullopt;
  return record_;
}

Tuple Tuple::with_schema(const Schema &schema) const {
  if (schema.size() != size())
    throw std::invalid_argument("Tuple schema has a different column count!");
  for (std::size_t i = 0; i < size(); ++i)
    if (schema[i].type != (*schema_)[i].type)
      throw std::invalid_argument("Tuple schema has different column types!");

  Tuple result = *this;
  result.schema_ = &schema;
  return result;
}

Tuple Tuple::project(const Schema &schema,
                     std::span<const std::size_t> positions,
                     std::span<std::size_t> resolved_columns) const {
  if (positions.size() != schema.size() ||
      resolved_columns.size() != positions.size())
    throw std::invalid_argument("Tuple projection storage has the wrong size!");
  for (std::size_t i = 0; i < positions.size(); ++i) {
    if (positions[i] >= size())
      throw std::out_of_range("Tuple projection position is out of range!");
    if (schema[i].type != (*schema_)[positions[i]].type)
      throw std::invalid_argument(
          "Tuple projection has different column types!");
  }
  for (std::size_t i = 0; i < positions.size(); ++i)
    resolved_columns[i] = physical_position(positions[i]);

  Tuple result = *this;
  result.schema_ = &schema;
  result.columns_ = resolved_columns;
  return result;
}

int compare_values(const Value &left, const Value &right) {
  if (left.index() != right.index())
    throw std::runtime_error("Cannot compare values of different types!");
  if (left < right)
    return -1;
  if (left > right)
    return 1;
  return 0;
}

std::size_t TupleHash::operator()(const Tuple &tuple) const {
  std::size_t tuple_hash = tuple.size();
  for (std::size_t i = 0; i < tuple.size(); ++i) {
    const std::size_t value_hash = std::hash<Value>{}(tuple.value_at(i));
    tuple_hash ^=
        value_hash + 0x9e3779b9 + (tuple_hash << 6) + (tuple_hash >> 2);
  }
  return tuple_hash;
}

bool TupleEqual::operator()(const Tuple &left, const Tuple &right) const {
  if (left.size() != right.size())
    return false;
  for (std::size_t i = 0; i < left.size(); ++i)
    if (compare_values(left.value_at(i), right.value_at(i)) != 0)
      return false;
  return true;
}

void validate_tuple(const Tuple &tuple, const Schema &schema) {
  if (schema.size() != tuple.size())
    throw std::runtime_error("Tuple size of " + std::to_string(tuple.size()) +
                             " does not match Schema size of " +
                             std::to_string(schema.size()));

  for (std::size_t i = 0; i < schema.size(); ++i) {
    const Type &source = tuple.schema()[i].type;
    const Type &target = schema[i].type;
    if (source.data_type() != target.data_type())
      throw std::runtime_error("Invalid tuple at position " +
                               std::to_string(i) +
                               (target.data_type() == DataType::INT
                                    ? "! Cannot match VARCHAR to INT"
                                    : "! Cannot match INT to VARCHAR"));
    if (target.data_type() == DataType::VARCHAR &&
        target.max_length() < source.max_length()) {
      const auto text = std::get<std::string_view>(tuple.value_at(i));
      if (text.size() > target.max_length())
        throw std::runtime_error(
            "Invalid tuple at position " + std::to_string(i) +
            "! VARCHAR of size " + std::to_string(text.size()) +
            " exceeds max size of " + std::to_string(target.max_length()));
    }
  }
}
} // namespace vql::algebra
