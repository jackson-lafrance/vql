#ifndef SWITCHER_H
#define SWITCHER_H

#include "schema.h"
#include "tuple.h"

#include <span>
#include <vector>

namespace vql::algebra::switcher {
std::vector<char> encode(const Tuple &tuple, const Schema &schema);
Tuple decode(std::span<const char> record, const Schema &schema);

int compare_records(std::span<const char> left, std::span<const char> right,
                    const Schema &schema, std::span<const SortKey> keys);
} // namespace vql::algebra::switcher

#endif // SWITCHER_H
