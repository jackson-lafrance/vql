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

class Schema {
public:
  Schema(std::vector<Column> columns);

  std::size_t size() const;
  const Column &operator[](std::size_t i) const;
  const Column &operator[](const std::string &name) const;
  std::size_t index_of(const std::string &name) const;

private:
  std::vector<Column> columns_;
};

Schema project_schema(const Schema &schema,
                      const std::vector<std::size_t> &positions);
Schema concat_schemas(const Schema &left, const Schema &right);

} // namespace vql::algebra

#endif // SCHEMA_H
