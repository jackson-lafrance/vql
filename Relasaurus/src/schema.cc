#include "schema.h"
#include "diagnostic.h"
#include <utility>

Schema::Schema(std::vector<Column> columns) : columns_(std::move(columns)) {
  for (std::size_t i{}; i < columns_.size(); ++i) {
    const std::string &name = columns_[i].name;
    if (index_by_name_.contains(name))
      throw DiagnosticError(DiagnosticCategory::Schema,
                            "DUPLICATE ATTRIBUTE: " + name);

    index_by_name_.emplace(name, i);
  }
}

const std::vector<Column> &Schema::columns() const { return columns_; }

std::size_t Schema::index_of(const std::string &name) const {
  auto index = index_by_name_.find(name);
  if (index == index_by_name_.end())
    return Schema::npos;

  return index->second;
}
 
 
