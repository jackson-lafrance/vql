#ifndef VQL_QUERY_SEMANTIC_ANALYZER_H
#define VQL_QUERY_SEMANTIC_ANALYZER_H

#include "ast.h"
#include "catalog.h"
#include "diagnostic.h"

#include <unordered_map>

namespace vql::query {

class SemanticAnalyzer {
public:
  explicit SemanticAnalyzer(const storage::Catalog &catalog);

  void analyze(Query &query) const;
  void analyze(Statement &statement) const;
  void analyze(Script &script) const;

private:
  struct ScopeEntry {
    std::string name;
    algebra::Schema schema;
  };

  using Scope = std::vector<ScopeEntry>;
  using Definitions = std::unordered_map<std::string, algebra::Schema>;

  algebra::Schema lookup_schema(const NameWithSpan &name,
                                const Definitions &definitions) const;
  void analyze_statement(Statement &statement, Definitions &definitions) const;
  algebra::Schema analyze_query(Query &query,
                                const Definitions &definitions) const;
  algebra::Schema analyze_select(SelectBlock &select,
                                 const Definitions &definitions) const;
  void add_table(TableRef &table, Scope &scope,
                 const Definitions &definitions) const;
  algebra::Type resolve_column(ColumnRef &column, const Scope &scope) const;
  algebra::Type resolve_aggregate(AggregateCall &aggregate,
                                  const Scope &scope) const;
  algebra::Type resolve_operand(Operand &operand, const Scope &scope,
                                bool allow_aggregates) const;
  void resolve_condition(Condition &condition, const Scope &scope,
                         bool allow_aggregates) const;
  void check_group_column(const ColumnRef &column,
                          const std::vector<ColumnRef> &keys) const;
  void finish_having(Condition &condition, const std::vector<ColumnRef> &keys,
                     std::vector<AggregateCall> &aggregates,
                     std::size_t &aggregate_number) const;

  const storage::Catalog &catalog_;
};

} // namespace vql::query

#endif // VQL_QUERY_SEMANTIC_ANALYZER_H
