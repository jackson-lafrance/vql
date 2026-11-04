#ifndef TUPLE_H
#define TUPLE_H
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <variant>
#include <vector>

namespace vql::algebra {
using Value = std::variant<std::int32_t, std::string>;

using Tuple = std::vector<Value>;

struct TupleHash {
  std::size_t operator()(const Tuple &tuple) const {
    std::size_t tuple_hash = tuple.size();
    for (const Value &value : tuple) {
      const std::size_t value_hash = std::hash<Value>{}(value);
      tuple_hash ^=
          value_hash + 0x9e3779b9 + (tuple_hash << 6) + (tuple_hash >> 2);
    }

    return tuple_hash;
  }
};
} // namespace vql::algebra

#endif // TUPLE_H
