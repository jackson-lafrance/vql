#ifndef SWITCHER_H
#define SWITCHER_H

#include "schema.h"
#include "tuple.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vql::algebra::switcher {
std::size_t encoded_size(const Tuple &tuple, const Schema &schema);
std::size_t encode_into(const Tuple &tuple, const Schema &schema,
                        std::span<char> destination);
std::vector<char> encode(const Tuple &tuple, const Schema &schema);
Tuple decode(std::span<const char> record, const Schema &schema);
void validate_record(std::span<const char> record, const Schema &schema);
std::span<const char> record_field_bytes(std::span<const char> record,
                                         const Schema &schema,
                                         std::size_t column);
std::int32_t record_int(std::span<const char> field);

int compare_records(std::span<const char> left, std::span<const char> right,
                    const Schema &schema, std::span<const SortKey> keys);
} // namespace vql::algebra::switcher

#endif // SWITCHER_H
