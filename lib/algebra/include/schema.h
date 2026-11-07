#ifndef SCHEMA_H
#define SCHEMA_H

#include <cstddef>
#include <string>
#include <vector>

namespace vql::algebra {

enum class DataType {
  INT,
  VARCHAR,
};

class Type {
public:
  friend bool operator==(const Type &left, const Type &right);

  Type(DataType data_type, std::size_t max_length = 0);

  DataType data_type() const;

  std::size_t max_length() const;

private:
  DataType data_type_;
  std::size_t max_length_;
};

struct Column {
  std::string name;
  Type type;
};

using Schema = std::vector<Column>;

} // namespace vql::algebra

#endif // SCHEMA_H
