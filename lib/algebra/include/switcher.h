#ifndef SWITCHER_H
#define SWITCHER_H

#include "schema.h"
#include "tuple.h"

#include <span>
#include <vector>

namespace vql::algebra::switcher {
// Both results own their data; decode does not retain the input span.
std::vector<char> encode(const Tuple &tuple, const Schema &schema);
Tuple decode(std::span<const char> record, const Schema &schema);
} // namespace vql::algebra::switcher

#endif // SWITCHER_H
