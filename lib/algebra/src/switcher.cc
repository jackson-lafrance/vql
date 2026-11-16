#include "switcher.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
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

} // namespace vql::algebra::switcher
