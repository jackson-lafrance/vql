#ifndef SCHEMA_H
#define SCHEMA_H

#include <cstddef>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

enum Type { STRING, NUMBER };

struct Column {
  std::string name;
  Type type;
};

class Schema {
public:
  static constexpr std::size_t npos =
      std::numeric_limits<std::size_t>::max();

  Schema(std::vector<Column> columns);

  const std::vector<Column> &columns() const;
  std::size_t index_of(const std::string &name) const;

private:
  std::vector<Column> columns_;
  std::unordered_map<std::string, std::size_t> index_by_name_;
};

#endif // SCHEMA_H
