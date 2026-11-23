#include "semantic_analyzer.h"

#include <unordered_set>
#include <utility>

namespace vql::query {

bool valid_sql_identifier(const std::string &name) {
  if (name.empty())
    return false;
  for (std::size_t i{0}; i < name.size(); ++i) {
    unsigned char character = static_cast<unsigned char>(name[i]);
    bool letter = (character >= 'a' && character <= 'z') ||
                  (character >= 'A' && character <= 'Z') || character == '_';
    if (!letter && (i == 0 || character < '0' || character > '9'))
      return false;
  }
  return true;
}

std::string column_name(const ColumnRef &column) {
  if (column.qualifier)
    return column.qualifier->name + "." + column.name.name;
  return column.name.name;
}

bool same_column(const ColumnRef &left, const ColumnRef &right) {
  if (!left.qualifier || !right.qualifier)
    return false;
  return left.qualifier->name == right.qualifier->name &&
         left.name.name == right.name.name &&
         left.resolved_position == right.resolved_position;
}

bool same_aggregate(const AggregateCall &left, const AggregateCall &right) {
  if (left.function != right.function ||
      left.argument.index() != right.argument.index())
    return false;
  if (std::holds_alternative<Star>(left.argument))
    return true;
  return same_column(std::get<ColumnRef>(left.argument),
                     std::get<ColumnRef>(right.argument));
}

SemanticAnalyzer::SemanticAnalyzer(const storage::Catalog &catalog)
    : catalog_(catalog) {}

void SemanticAnalyzer::analyze(Query &query) const {
  Definitions definitions;
  analyze_query(query, definitions);
}

void SemanticAnalyzer::analyze(Statement &statement) const {
  Definitions definitions;
  analyze_statement(statement, definitions);
}

void SemanticAnalyzer::analyze(Script &script) const {
  Definitions definitions;
  for (Statement &statement : script.statements)
    analyze_statement(statement, definitions);
}

algebra::Schema
SemanticAnalyzer::lookup_schema(const NameWithSpan &name,
                                const Definitions &definitions) const {
  auto definition = definitions.find(name.name);
  if (definition != definitions.end())
    return definition->second;
  try {
    return catalog_.get_table(name.name).schema;
  } catch (const std::out_of_range &) {
    throw DiagnosticError(ErrorCategory::NAME,
                          "Unknown table '" + name.name + "'", name.span);
  } catch (const std::runtime_error &error) {
    throw DiagnosticError(
        ErrorCategory::SCHEMA,
        "Cannot read table '" + name.name + "': " + error.what(), name.span);
  }
}

void SemanticAnalyzer::analyze_statement(Statement &statement,
                                         Definitions &definitions) const {
  if (auto *query = std::get_if<Query>(&statement)) {
    analyze_query(*query, definitions);
    return;
  }
  if (auto *insert = std::get_if<Insert>(&statement)) {
    algebra::Schema schema = lookup_schema(insert->table, definitions);
    if (insert->values.size() != schema.size())
      throw DiagnosticError(ErrorCategory::SCHEMA,
                            "INSERT value count does not match table '" +
                                insert->table.name + "'",
                            insert->span);
    for (std::size_t i{0}; i < schema.size(); ++i) {
      const Literal &literal = insert->values[i];
      bool integer = std::holds_alternative<std::int32_t>(literal.value);
      if (integer != (schema[i].type.data_type() == algebra::DataType::INT))
        throw DiagnosticError(ErrorCategory::TYPE,
                              "Wrong value type for column '" + schema[i].name +
                                  "'",
                              literal.span);
      if (!integer && std::get<std::string>(literal.value).size() >
                          schema[i].type.max_length())
        throw DiagnosticError(ErrorCategory::TYPE,
                              "Value exceeds VARCHAR length for column '" +
                                  schema[i].name + "'",
                              literal.span);
    }
    return;
  }

  auto &create = std::get<CreateTable>(statement);
  create.resolved_schema.reset();
  if (!valid_sql_identifier(create.table.name))
    throw DiagnosticError(ErrorCategory::SCHEMA, "Invalid table name",
                          create.table.span);
  if (create.table.name == "catalog")
    throw DiagnosticError(ErrorCategory::SCHEMA,
                          "Table name 'catalog' conflicts with catalog.db",
                          create.table.span);
  bool exists = definitions.contains(create.table.name);
  if (!exists) {
    try {
      catalog_.get_table(create.table.name);
      exists = true;
    } catch (const std::out_of_range &) {
      // A missing table is required for CREATE TABLE.
    } catch (const std::runtime_error &error) {
      throw DiagnosticError(ErrorCategory::SCHEMA, error.what(),
                            create.table.span);
    }
  }
  if (exists)
    throw DiagnosticError(ErrorCategory::SCHEMA,
                          "Table '" + create.table.name + "' already exists",
                          create.table.span);
  if (create.columns.empty())
    throw DiagnosticError(ErrorCategory::SCHEMA,
                          "CREATE TABLE requires at least one column",
                          create.span);

  std::vector<algebra::Column> columns;
  std::unordered_set<std::string> names;
  for (const ColumnDefinition &column : create.columns) {
    if (!valid_sql_identifier(column.name.name))
      throw DiagnosticError(ErrorCategory::SCHEMA, "Invalid column name",
                            column.name.span);
    if (!names.insert(column.name.name).second)
      throw DiagnosticError(ErrorCategory::SCHEMA,
                            "Duplicate column '" + column.name.name + "'",
                            column.name.span);
    if (const auto *varchar = std::get_if<VarcharType>(&column.type.value)) {
      if (varchar->length < 1 || varchar->length > 255)
        throw DiagnosticError(ErrorCategory::SCHEMA,
                              "VARCHAR length must be between 1 and 255",
                              column.type.span);
      columns.push_back(
          {column.name.name,
           algebra::Type(algebra::DataType::VARCHAR,
                         static_cast<std::size_t>(varchar->length))});
    } else {
      columns.push_back(
          {column.name.name, algebra::Type(algebra::DataType::INT)});
    }
  }
  algebra::Schema schema(std::move(columns));
  create.resolved_schema = schema;
  definitions.emplace(create.table.name, std::move(schema));
}

algebra::Schema
SemanticAnalyzer::analyze_query(Query &query,
                                const Definitions &definitions) const {
  query.output_schema.reset();
  if (auto *select = std::get_if<SelectBlock>(&query.value)) {
    query.output_schema = analyze_select(*select, definitions);
  } else {
    auto &operation = std::get<SetOperation>(query.value);
    if (!operation.left || !operation.right)
      throw DiagnosticError(ErrorCategory::SYNTAX,
                            "Set operation requires two queries", query.span);
    algebra::Schema left = analyze_query(*operation.left, definitions);
    algebra::Schema right = analyze_query(*operation.right, definitions);
    try {
      algebra::validate_set_schemas(left, right);
    } catch (const std::runtime_error &error) {
      throw DiagnosticError(ErrorCategory::SCHEMA, error.what(),
                            operation.operator_span);
    }
    query.output_schema = std::move(left);
  }
  return *query.output_schema;
}

void SemanticAnalyzer::add_table(TableRef &table, Scope &scope,
                                 const Definitions &definitions) const {
  table.output_schema.reset();
  NameWithSpan name;
  if (auto *named = std::get_if<NamedTable>(&table.value)) {
    name = named->alias ? *named->alias : named->name;
  } else {
    name = std::get<SubqueryTable>(table.value).alias;
  }
  if (name.name.empty())
    throw DiagnosticError(ErrorCategory::NAME, "Missing table alias or name",
                          table.span);
  for (const ScopeEntry &entry : scope)
    if (entry.name == name.name)
      throw DiagnosticError(ErrorCategory::NAME,
                            "Duplicate table alias '" + name.name + "'",
                            name.span);

  if (auto *named = std::get_if<NamedTable>(&table.value)) {
    table.output_schema = lookup_schema(named->name, definitions);
  } else {
    auto &subquery = std::get<SubqueryTable>(table.value);
    if (!subquery.query)
      throw DiagnosticError(ErrorCategory::SYNTAX, "Missing subquery",
                            table.span);
    table.output_schema = analyze_query(*subquery.query, definitions);
  }
  scope.push_back({name.name, *table.output_schema});
}

algebra::Type SemanticAnalyzer::resolve_column(ColumnRef &column,
                                               const Scope &scope) const {
  column.resolved_type.reset();
  column.resolved_position.reset();
  const ScopeEntry *match = nullptr;
  std::size_t position = 0;
  std::size_t matches = 0;
  bool qualifier_found = false;
  std::string candidates;
  for (const ScopeEntry &entry : scope) {
    if (column.qualifier && column.qualifier->name != entry.name)
      continue;
    qualifier_found = true;
    for (std::size_t i{0}; i < entry.schema.size(); ++i) {
      if (entry.schema[i].name != column.name.name)
        continue;
      if (!candidates.empty())
        candidates += ", ";
      candidates += entry.name + "." + column.name.name + " (column " +
                    std::to_string(i + 1) + ")";
      match = &entry;
      position = i;
      ++matches;
    }
  }
  if (column.qualifier && !qualifier_found)
    throw DiagnosticError(ErrorCategory::NAME,
                          "Unknown table or alias '" + column.qualifier->name +
                              "'",
                          column.qualifier->span);
  if (matches == 0)
    throw DiagnosticError(ErrorCategory::NAME,
                          "Unknown column '" + column_name(column) + "'",
                          column.name.span);
  if (matches > 1)
    throw DiagnosticError(ErrorCategory::AMBIGUITY,
                          "Ambiguous column '" + column_name(column) +
                              "'; candidates: " + candidates,
                          column.span);
  if (!column.qualifier)
    column.qualifier = NameWithSpan{match->name, column.span};
  column.resolved_position = position;
  column.resolved_type = match->schema[position].type;
  return *column.resolved_type;
}

algebra::Type SemanticAnalyzer::resolve_aggregate(AggregateCall &aggregate,
                                                  const Scope &scope) const {
  aggregate.resolved_type.reset();
  if (auto *column = std::get_if<ColumnRef>(&aggregate.argument)) {
    algebra::Type input = resolve_column(*column, scope);
    switch (aggregate.function) {
    case AggregateFunction::COUNT:
      aggregate.resolved_type = algebra::Type(algebra::DataType::INT);
      break;
    case AggregateFunction::SUM:
    case AggregateFunction::AVG:
      if (input.data_type() != algebra::DataType::INT)
        throw DiagnosticError(ErrorCategory::TYPE,
                              "SUM and AVG require an INT column",
                              aggregate.span);
      if (aggregate.function == AggregateFunction::AVG)
        aggregate.resolved_type = algebra::Type(algebra::DataType::VARCHAR, 16);
      else
        aggregate.resolved_type = algebra::Type(algebra::DataType::INT);
      break;
    case AggregateFunction::MIN:
    case AggregateFunction::MAX:
      aggregate.resolved_type = input;
      break;
    }
  } else {
    if (aggregate.function != AggregateFunction::COUNT)
      throw DiagnosticError(ErrorCategory::AGGREGATE, "Only COUNT accepts '*'",
                            aggregate.span);
    aggregate.resolved_type = algebra::Type(algebra::DataType::INT);
  }
  if (!aggregate.resolved_type)
    throw DiagnosticError(ErrorCategory::SYNTAX, "Unknown aggregate function",
                          aggregate.span);
  return *aggregate.resolved_type;
}

algebra::Type SemanticAnalyzer::resolve_operand(Operand &operand,
                                                const Scope &scope,
                                                bool allow_aggregates) const {
  if (auto *column = std::get_if<ColumnRef>(&operand))
    return resolve_column(*column, scope);
  if (auto *literal = std::get_if<Literal>(&operand)) {
    if (std::holds_alternative<std::int32_t>(literal->value))
      return algebra::Type(algebra::DataType::INT);
    // Comparisons check INT versus VARCHAR, not literal string capacity.
    return algebra::Type(algebra::DataType::VARCHAR, 255);
  }
  auto &aggregate = std::get<AggregateCall>(operand);
  if (!allow_aggregates)
    throw DiagnosticError(ErrorCategory::AGGREGATE,
                          "Aggregates are not allowed in WHERE or JOIN ON",
                          aggregate.span);
  return resolve_aggregate(aggregate, scope);
}

void SemanticAnalyzer::resolve_condition(Condition &condition,
                                         const Scope &scope,
                                         bool allow_aggregates) const {
  if (auto *comparison = std::get_if<Comparison>(&condition.value)) {
    algebra::Type left =
        resolve_operand(comparison->left, scope, allow_aggregates);
    algebra::Type right =
        resolve_operand(comparison->right, scope, allow_aggregates);
    if (left.data_type() != right.data_type())
      throw DiagnosticError(ErrorCategory::TYPE,
                            "Cannot compare INT with VARCHAR",
                            comparison->operator_span);
  } else if (auto *negation = std::get_if<NotCondition>(&condition.value)) {
    if (!negation->child)
      throw DiagnosticError(ErrorCategory::SYNTAX, "NOT requires a condition",
                            condition.span);
    resolve_condition(*negation->child, scope, allow_aggregates);
  } else {
    auto &logical = std::get<LogicalCondition>(condition.value);
    if (!logical.left || !logical.right)
      throw DiagnosticError(ErrorCategory::SYNTAX,
                            "AND and OR require two conditions",
                            condition.span);
    resolve_condition(*logical.left, scope, allow_aggregates);
    resolve_condition(*logical.right, scope, allow_aggregates);
  }
}

void SemanticAnalyzer::check_group_column(
    const ColumnRef &column, const std::vector<ColumnRef> &keys) const {
  for (const ColumnRef &key : keys)
    if (same_column(column, key))
      return;
  throw DiagnosticError(ErrorCategory::AGGREGATE,
                        "Column '" + column_name(column) +
                            "' is not a grouping column",
                        column.span);
}

void SemanticAnalyzer::finish_having(Condition &condition,
                                     const std::vector<ColumnRef> &keys,
                                     std::vector<AggregateCall> &aggregates,
                                     std::size_t &aggregate_number) const {
  if (auto *comparison = std::get_if<Comparison>(&condition.value)) {
    for (Operand *operand : {&comparison->left, &comparison->right}) {
      if (auto *column = std::get_if<ColumnRef>(operand)) {
        check_group_column(*column, keys);
      } else if (auto *aggregate = std::get_if<AggregateCall>(operand)) {
        bool found = false;
        for (const AggregateCall &known : aggregates) {
          if (same_aggregate(*aggregate, known)) {
            aggregate->output_name = known.output_name;
            found = true;
            break;
          }
        }
        if (!found) {
          aggregate->output_name = "agg_" + std::to_string(++aggregate_number);
          aggregates.push_back(*aggregate);
        }
      }
    }
  } else if (auto *negation = std::get_if<NotCondition>(&condition.value)) {
    finish_having(*negation->child, keys, aggregates, aggregate_number);
  } else {
    auto &logical = std::get<LogicalCondition>(condition.value);
    finish_having(*logical.left, keys, aggregates, aggregate_number);
    finish_having(*logical.right, keys, aggregates, aggregate_number);
  }
}

algebra::Schema
SemanticAnalyzer::analyze_select(SelectBlock &select,
                                 const Definitions &definitions) const {
  Scope scope;
  if (auto *comma = std::get_if<CommaFrom>(&select.from)) {
    if (comma->tables.empty())
      throw DiagnosticError(ErrorCategory::SYNTAX, "FROM requires a table",
                            select.span);
    for (TableRef &table : comma->tables)
      add_table(table, scope, definitions);
  } else {
    auto &joined = std::get<JoinFrom>(select.from);
    add_table(joined.first, scope, definitions);
    if (joined.joins.empty())
      throw DiagnosticError(ErrorCategory::SYNTAX,
                            "JOIN requires a right table", select.span);
    for (JoinClause &join : joined.joins) {
      add_table(join.right, scope, definitions);
      resolve_condition(join.on, scope, false);
    }
  }

  if (select.where)
    resolve_condition(*select.where, scope, false);

  std::vector<ColumnRef> keys;
  if (select.group_by) {
    if (select.group_by->columns.empty())
      throw DiagnosticError(ErrorCategory::SYNTAX, "GROUP BY requires a column",
                            select.group_by->span);
    for (ColumnRef &column : select.group_by->columns) {
      resolve_column(column, scope);
      keys.push_back(column);
    }
  }

  bool expanded = std::holds_alternative<Star>(select.columns);
  if (expanded) {
    Span span = std::get<Star>(select.columns).span;
    std::vector<SelectItem> items;
    for (const ScopeEntry &entry : scope) {
      for (std::size_t i{0}; i < entry.schema.size(); ++i) {
        ColumnRef column{};
        column.qualifier = NameWithSpan{entry.name, span};
        column.name = NameWithSpan{entry.schema[i].name, span};
        column.span = span;
        column.resolved_type = entry.schema[i].type;
        column.resolved_position = i;
        SelectItem item{};
        item.expression = std::move(column);
        item.span = span;
        items.push_back(std::move(item));
      }
    }
    select.columns = std::move(items);
  }
  auto &items = std::get<std::vector<SelectItem>>(select.columns);
  if (items.empty())
    throw DiagnosticError(ErrorCategory::SYNTAX, "SELECT requires an item",
                          select.span);

  select.aggregating = select.group_by.has_value();
  for (const SelectItem &item : items)
    if (std::holds_alternative<AggregateCall>(item.expression))
      select.aggregating = true;

  std::size_t aggregate_number = 0;
  std::vector<AggregateCall> aggregates;
  std::vector<algebra::Column> output;
  for (SelectItem &item : items) {
    if (auto *column = std::get_if<ColumnRef>(&item.expression)) {
      if (!expanded)
        resolve_column(*column, scope);
      if (select.aggregating)
        check_group_column(*column, keys);
      item.output_name = item.alias ? item.alias->name : column->name.name;
      output.push_back({item.output_name, *column->resolved_type});
    } else {
      auto &aggregate = std::get<AggregateCall>(item.expression);
      algebra::Type type = resolve_aggregate(aggregate, scope);
      ++aggregate_number;
      aggregate.output_name = item.alias
                                  ? item.alias->name
                                  : "agg_" + std::to_string(aggregate_number);
      item.output_name = aggregate.output_name;
      output.push_back({item.output_name, type});
      aggregates.push_back(aggregate);
    }
  }

  if (select.group_by && select.group_by->having) {
    resolve_condition(*select.group_by->having, scope, true);
    finish_having(*select.group_by->having, keys, aggregates, aggregate_number);
  }

  for (SortKey &key : select.order_by) {
    key.output_position.reset();
    if (!key.column.qualifier) {
      std::size_t matches = 0;
      std::size_t position = 0;
      for (std::size_t i{0}; i < items.size(); ++i) {
        bool output_alias =
            items[i].alias.has_value() ||
            std::holds_alternative<AggregateCall>(items[i].expression);
        if (output_alias && items[i].output_name == key.column.name.name) {
          position = i;
          ++matches;
        }
      }
      if (matches > 1)
        throw DiagnosticError(ErrorCategory::AMBIGUITY,
                              "Ambiguous output alias '" +
                                  key.column.name.name + "'",
                              key.column.span);
      if (matches == 1) {
        key.output_position = position;
        key.column.resolved_type = output[position].type;
        key.column.resolved_position.reset();
        continue;
      }
    }
    resolve_column(key.column, scope);
    if (select.aggregating)
      check_group_column(key.column, keys);
    if (select.distinct) {
      bool selected = false;
      for (const SelectItem &item : items) {
        const auto *column = std::get_if<ColumnRef>(&item.expression);
        if (column && same_column(*column, key.column))
          selected = true;
      }
      if (!selected)
        throw DiagnosticError(
            ErrorCategory::SCHEMA,
            "SELECT DISTINCT cannot ORDER BY a column outside its select list",
            key.column.span);
    }
  }
  if (select.limit && select.limit->count < 0)
    throw DiagnosticError(ErrorCategory::SCHEMA, "LIMIT must not be negative",
                          select.limit->span);
  return algebra::Schema(std::move(output));
}

} // namespace vql::query
