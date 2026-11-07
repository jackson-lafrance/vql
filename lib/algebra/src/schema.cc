#include "schema.h"

#include <stdexcept>

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

} // namespace vql::algebra
