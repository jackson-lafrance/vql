#ifndef TUPLE_H
#define TUPLE_H
#include "schema.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <variant>
#include <vector>

namespace vql::algebra {
using Value = std::variant<std::int32_t, std::string>;
int compare_values(const Value &left, const Value &right);

class Tuple {
public:
  Tuple() = default;
  explicit Tuple(std::vector<Value> values);

  std::size_t size() const;
  Value operator[](std::size_t i) const;
  // just for borrowing the value instead of returning a copy
  const Value &value_at(std::size_t i) const;

private:
  std::vector<Value> values_;
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
Tuple project_tuple(const Tuple &tuple,
                    const std::vector<std::size_t> &positions);
Tuple concat_tuples(const Tuple &left, const Tuple &right);
} // namespace vql::algebra

#endif // TUPLE_H
