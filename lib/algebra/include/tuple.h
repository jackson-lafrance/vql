#ifndef TUPLE_H
#define TUPLE_H
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace vql::algebra {
using Value = std::variant<std::int32_t, std::string>;
bool operator==(const Value &left, const Value &right);
bool operator!=(const Value &left, const Value &right);
bool operator<(const Value &left, const Value &right);
struct ValueHash {
  std::size_t operator()(const Value&) const;
};

using Tuple = std::vector<Value>;
bool operator==(const Tuple &left, const Tuple &right);
bool operator!=(const Tuple &left, const Tuple &right);
bool operator<(const Tuple &left, const Tuple &right);
struct TupleHash {
  std::size_t operator()(const Tuple&) const;
};
} // namespace vql::algebra

#endif // TUPLE_H
