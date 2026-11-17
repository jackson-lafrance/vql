#ifndef TUPLE_H
#define TUPLE_H

#include "schema.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

namespace vql::algebra {
using Value = std::variant<std::int32_t, std::string_view>;
int compare_values(const Value &left, const Value &right);

class Tuple {
public:
  Tuple(const Schema &schema, std::span<const char> record);

  std::size_t size() const;
  const Schema &schema() const;
  Value operator[](std::size_t i) const;
  Value value_at(std::size_t i) const;

  std::optional<std::span<const char>> encoded_record() const;
  Tuple with_schema(const Schema &schema) const;

  Tuple project(const Schema &schema, std::span<const std::size_t> positions,
                std::span<std::size_t> resolved_columns) const;

private:
  std::size_t physical_position(std::size_t i) const;

  const Schema *schema_;
  const Schema *record_schema_;
  std::span<const char> record_;
  std::span<const std::size_t> columns_;
};

struct SortKey {
  std::size_t position;
  bool ascending{true};
};

struct TupleHash {
  std::size_t operator()(const Tuple &tuple) const;
};

struct TupleEqual {
  bool operator()(const Tuple &left, const Tuple &right) const;
};

void validate_tuple(const Tuple &tuple, const Schema &schema);
} // namespace vql::algebra

#endif // TUPLE_H
