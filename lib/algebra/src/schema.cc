#include "schema.h"

#include <stdexcept>
#include <utility>

namespace vql::algebra {

bool operator==(const Type &left, const Type &right) {
  if (left.data_type_ != right.data_type_) {
    return false;
  }

  if (left.data_type_ == DataType::VARCHAR) {
    return left.max_length_ == right.max_length_;
  }

  return true;
}

Type::Type(DataType data_type, std::size_t max_length) {
  if (data_type == DataType::INT) {
    data_type_ = DataType::INT;
    max_length_ = 0;
  } else {
    data_type_ = DataType::VARCHAR;
    if (max_length < 1 || max_length > 255) {
      throw std::runtime_error("Max length must be between 1 and 255!");
    }
    max_length_ = max_length;
  }
}

DataType Type::data_type() const { return data_type_; }

std::size_t Type::max_length() const {
  if (data_type_ == DataType::INT) {
    throw std::logic_error("Integers do not have a max length!");
  }

  return max_length_;
}

Schema::Schema(std::vector<Column> columns) : columns_(std::move(columns)) {}

std::size_t Schema::size() const { return columns_.size(); }

const Column &Schema::operator[](std::size_t i) const {
  if (i >= columns_.size())
    throw std::runtime_error("Access out of bounds to {" + std::to_string(i) +
                             "} to schema of size " +
                             std::to_string(columns_.size()) + "!");

  return columns_[i];
}

const Column &Schema::operator[](const std::string &name) const {
  return columns_[index_of(name)];
}

std::size_t Schema::index_of(const std::string &name) const {
  std::size_t match = columns_.size();
  for (std::size_t i{0}; i < columns_.size(); ++i) {
    if (columns_[i].name == name) {
      if (match != columns_.size())
        throw std::runtime_error("Cannot return ambiguous column named \"" +
                                 name + "\"");
      match = i;
    }
  }

  if (match == columns_.size())
    throw std::runtime_error("Cannot find column named \"" + name +
                             "\" in schema");

  return match;
}

Schema project_schema(const Schema &schema,
                      const std::vector<std::size_t> &positions) {
  std::vector<Column> columns;
  columns.reserve(positions.size());

  for (std::size_t position : positions)
    columns.push_back(schema[position]);

  return Schema(std::move(columns));
}

Schema concat_schemas(const Schema &left, const Schema &right) {
  std::vector<Column> columns;
  columns.reserve(left.size() + right.size());

  for (std::size_t i{0}; i < left.size(); ++i)
    columns.push_back(left[i]);
  for (std::size_t i{0}; i < right.size(); ++i)
    columns.push_back(right[i]);

  return Schema(std::move(columns));
}

} // namespace vql::algebra
