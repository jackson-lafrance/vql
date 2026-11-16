#include "switcher.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace vql::algebra::switcher {

std::vector<char> encode(const Tuple &tuple, const Schema &schema) {
  validate_tuple(tuple, schema);

  std::vector<char> record;
  for (std::size_t i{0}; i < schema.size(); ++i) {
    const Value value = tuple[i];
    if (schema[i].type.data_type() == DataType::INT) {
      const unsigned int bits =
          std::bit_cast<std::uint32_t>(std::get<std::int32_t>(value));
      for (unsigned shift{0}; shift < 32; shift += 8)
        record.push_back(static_cast<char>((bits >> shift) & 0xFF));
    } else {
      const std::string &text = std::get<std::string>(value);
      const auto length = static_cast<std::uint16_t>(text.size());
      record.push_back(static_cast<char>(length & 0xFF));
      record.push_back(static_cast<char>((length >> 8) & 0xFF));
      record.insert(record.end(), text.begin(), text.end());
    }
  }

  return record;
}

Tuple decode(std::span<const char> record, const Schema &schema) {
  std::size_t position{0};
  std::vector<Value> values;
  values.reserve(schema.size());

  for (std::size_t i{0}; i < schema.size(); ++i) {
    if (schema[i].type.data_type() == DataType::INT) {
      if (position > record.size() || record.size() - position < 4)
        throw std::runtime_error("INT too short!");

      values.emplace_back(
          std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(
              static_cast<unsigned char>(record[position]) |
              (static_cast<unsigned char>(record[position + 1]) << 8) |
              (static_cast<unsigned char>(record[position + 2]) << 16) |
              (static_cast<unsigned char>(record[position + 3]) << 24))));
      position += 4;
    } else {
      if (position > record.size() || record.size() - position < 2)
        throw std::runtime_error("VARCHAR too short!");

      std::uint16_t length =
          static_cast<unsigned char>(record[position]) |
          (static_cast<unsigned char>(record[position + 1]) << 8);
      position += 2;

      if (length > record.size() - position ||
          length > schema[i].type.max_length())
        throw std::runtime_error("Malformed VARCHAR!");

      values.emplace_back(std::string(record.data() + position, length));
      position += length;
    }
  }

  if (position != record.size())
    throw std::runtime_error("Unexpected bytes after record!");

  return Tuple(std::move(values));
}

std::span<const char> record_field_bytes(std::span<const char> record,
                                         const Schema &schema,
                                         std::size_t column) {
  if (column >= schema.size())
    throw std::out_of_range("Sort key position is out of range!");

  std::size_t position = 0;
  std::span<const char> field;
  for (std::size_t i = 0; i < schema.size(); ++i) {
    std::size_t length = 4;
    if (schema[i].type.data_type() == DataType::VARCHAR) {
      if (position > record.size() || record.size() - position < 2)
        throw std::runtime_error("VARCHAR too short!");
      length = static_cast<unsigned char>(record[position]) |
               (static_cast<unsigned char>(record[position + 1]) << 8);
      position += 2;
      if (length > schema[i].type.max_length())
        throw std::runtime_error("Malformed VARCHAR!");
    }

    if (position > record.size() || length > record.size() - position)
      throw std::runtime_error("Truncated record field!");
    if (i == column)
      field = record.subspan(position, length);
    position += length;
  }
  if (position != record.size())
    throw std::runtime_error("Unexpected bytes after record!");

  return field;
}

std::int32_t record_int(std::span<const char> field) {
  if (field.size() != 4)
    throw std::runtime_error("INT must contain four bytes!");

  const std::uint32_t bits =
      std::uint32_t{static_cast<unsigned char>(field[0])} |
      (std::uint32_t{static_cast<unsigned char>(field[1])} << 8) |
      (std::uint32_t{static_cast<unsigned char>(field[2])} << 16) |
      (std::uint32_t{static_cast<unsigned char>(field[3])} << 24);
  return std::bit_cast<std::int32_t>(bits);
}

int compare_record_fields(std::span<const char> left,
                          std::span<const char> right, const Type &type) {
  if (type.data_type() == DataType::INT)
    return compare_values(record_int(left), record_int(right));

  const int result = std::string_view(left.data(), left.size())
                         .compare(std::string_view(right.data(), right.size()));
  if (result < 0)
    return -1;
  if (result > 0)
    return 1;
  return 0;
}

int compare_records(std::span<const char> left, std::span<const char> right,
                    const Schema &schema, std::span<const SortKey> keys) {
  for (const SortKey &key : keys)
    if (key.position >= schema.size())
      throw std::out_of_range("Sort key position is out of range!");

  for (const SortKey &key : keys)
    if (const int result = compare_record_fields(
            record_field_bytes(left, schema, key.position),
            record_field_bytes(right, schema, key.position),
            schema[key.position].type);
        result != 0)
      return key.ascending ? result : -result;

  return 0;
}

} // namespace vql::algebra::switcher
