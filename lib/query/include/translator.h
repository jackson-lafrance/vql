#ifndef VQL_QUERY_TRANSLATOR_H
#define VQL_QUERY_TRANSLATOR_H

#include "ast.h"

#include <json/json.h>

namespace vql::query {

class Translator {
public:
  Json::Value translate(const Query &query) const;

private:
  struct AggregateBinding {
    const AggregateCall *aggregate;
    std::string name;
  };

  Json::Value translate_select(const SelectBlock &select) const;
  Json::Value translate_table(const TableRef &table) const;
  Json::Value translate_from(const FromClause &from) const;

  std::string column_text(const ColumnRef &column) const;
  Json::Value
  condition_json(const Condition &condition,
                 const std::vector<AggregateBinding> &aggregates) const;
  std::string aggregate_function(AggregateFunction function) const;
  bool columns_match(const ColumnRef &left, const ColumnRef &right) const;
  bool aggregates_match(const AggregateCall &left,
                        const AggregateCall &right) const;
  std::size_t
  aggregate_position(const AggregateCall &aggregate,
                     const std::vector<AggregateBinding> &aggregates) const;
  void add_aggregate(const AggregateCall &aggregate,
                     const std::vector<ColumnRef> &keys,
                     std::vector<AggregateBinding> &aggregates) const;
  void collect_having(const Condition &condition,
                      const std::vector<ColumnRef> &keys,
                      std::vector<AggregateBinding> &aggregates) const;

  std::string table_name(const TableRef &table) const;
  void append_input_names(const TableRef &table,
                          std::vector<std::string> &names) const;
  std::size_t source_position(const ColumnRef &column,
                              const FromClause &from) const;
};

} // namespace vql::query

#endif // VQL_QUERY_TRANSLATOR_H
