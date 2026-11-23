#include "translator.h"

#include <stdexcept>
#include <utility>

namespace vql::query {

Json::Value logical_unary(const std::string &op, Json::Value child) {
  Json::Value node(Json::objectValue);
  node["op"] = op;
  node["child"] = std::move(child);
  return node;
}

bool repeated_plan_name(const std::vector<std::string> &names,
                        const std::string &name) {
  std::size_t matches = 0;
  for (const std::string &candidate : names)
    if (candidate == name)
      ++matches;
  return matches > 1;
}

std::string Translator::column_text(const ColumnRef &column) const {
  if (!column.qualifier || !column.resolved_type || !column.resolved_position)
    throw std::logic_error("Translation requires a resolved column!");
  return column.qualifier->name + "." + column.name.name;
}

std::string Translator::literal_text(const Literal &literal) const {
  if (const auto *integer = std::get_if<std::int32_t>(&literal.value))
    return std::to_string(*integer);
  std::string text = "'";
  for (char character : std::get<std::string>(literal.value)) {
    if (character == '\'')
      text += '\'';
    text += character;
  }
  text += '\'';
  return text;
}

bool Translator::columns_match(const ColumnRef &left,
                               const ColumnRef &right) const {
  return left.qualifier && right.qualifier && left.resolved_position &&
         right.resolved_position &&
         left.qualifier->name == right.qualifier->name &&
         left.name.name == right.name.name &&
         left.resolved_position == right.resolved_position;
}

bool Translator::aggregates_match(const AggregateCall &left,
                                  const AggregateCall &right) const {
  if (left.function != right.function ||
      left.argument.index() != right.argument.index())
    return false;
  if (std::holds_alternative<Star>(left.argument))
    return true;
  return columns_match(std::get<ColumnRef>(left.argument),
                       std::get<ColumnRef>(right.argument));
}

std::string Translator::aggregate_function(AggregateFunction function) const {
  switch (function) {
  case AggregateFunction::COUNT:
    return "count";
  case AggregateFunction::SUM:
    return "sum";
  case AggregateFunction::AVG:
    return "avg";
  case AggregateFunction::MIN:
    return "min";
  case AggregateFunction::MAX:
    return "max";
  }
  throw std::logic_error("Unknown aggregate function during translation!");
}

std::size_t Translator::aggregate_position(
    const AggregateCall &aggregate,
    const std::vector<AggregateBinding> &aggregates) const {
  for (std::size_t i{0}; i < aggregates.size(); ++i)
    if (aggregates[i].aggregate == &aggregate)
      return i;
  for (std::size_t i{0}; i < aggregates.size(); ++i) {
    const AggregateCall &known = *aggregates[i].aggregate;
    if (known.output_name == aggregate.output_name &&
        aggregates_match(known, aggregate))
      return i;
  }
  throw std::logic_error("HAVING aggregate has no GROUP BY binding!");
}

void Translator::add_aggregate(
    const AggregateCall &aggregate, const std::vector<ColumnRef> &keys,
    std::vector<AggregateBinding> &aggregates) const {
  if (!aggregate.resolved_type || aggregate.output_name.empty())
    throw std::logic_error("Translation requires a resolved aggregate!");
  std::string name = aggregate.output_name;
  std::size_t suffix = aggregates.size() + 1;
  for (;;) {
    bool used = false;
    for (const ColumnRef &key : keys)
      if (column_text(key) == name)
        used = true;
    for (const AggregateBinding &binding : aggregates)
      if (binding.name == name)
        used = true;
    if (!used)
      break;
    name = "_vql_agg_" + std::to_string(suffix++);
  }
  aggregates.push_back({&aggregate, std::move(name)});
}

void Translator::collect_having(
    const Condition &condition, const std::vector<ColumnRef> &keys,
    std::vector<AggregateBinding> &aggregates) const {
  if (const auto *comparison = std::get_if<Comparison>(&condition.value)) {
    for (const Operand *operand : {&comparison->left, &comparison->right}) {
      const auto *aggregate = std::get_if<AggregateCall>(operand);
      if (!aggregate)
        continue;
      bool found = false;
      for (const AggregateBinding &binding : aggregates)
        if (binding.aggregate->output_name == aggregate->output_name &&
            aggregates_match(*binding.aggregate, *aggregate))
          found = true;
      if (!found)
        add_aggregate(*aggregate, keys, aggregates);
    }
  } else if (const auto *negation =
                 std::get_if<NotCondition>(&condition.value)) {
    if (!negation->child)
      throw std::logic_error("Cannot translate NOT without a child!");
    collect_having(*negation->child, keys, aggregates);
  } else {
    const auto &logical = std::get<LogicalCondition>(condition.value);
    if (!logical.left || !logical.right)
      throw std::logic_error(
          "Cannot translate a condition with missing children!");
    collect_having(*logical.left, keys, aggregates);
    collect_having(*logical.right, keys, aggregates);
  }
}

std::string Translator::operand_text(
    const Operand &operand,
    const std::vector<AggregateBinding> &aggregates) const {
  if (const auto *column = std::get_if<ColumnRef>(&operand))
    return column_text(*column);
  if (const auto *literal = std::get_if<Literal>(&operand))
    return literal_text(*literal);
  const auto &aggregate = std::get<AggregateCall>(operand);
  return aggregates[aggregate_position(aggregate, aggregates)].name;
}

std::string Translator::condition_text(
    const Condition &condition,
    const std::vector<AggregateBinding> &aggregates) const {
  if (const auto *comparison = std::get_if<Comparison>(&condition.value)) {
    std::string op;
    switch (comparison->op) {
    case ComparisonOperator::EQUAL:
      op = "=";
      break;
    case ComparisonOperator::NOT_EQUAL:
      op = "<>";
      break;
    case ComparisonOperator::LESS_THAN:
      op = "<";
      break;
    case ComparisonOperator::LESS_THAN_OR_EQUAL:
      op = "<=";
      break;
    case ComparisonOperator::GREATER_THAN:
      op = ">";
      break;
    case ComparisonOperator::GREATER_THAN_OR_EQUAL:
      op = ">=";
      break;
    default:
      throw std::logic_error("Unknown comparison operator during translation!");
    }
    return operand_text(comparison->left, aggregates) + " " + op + " " +
           operand_text(comparison->right, aggregates);
  }
  if (const auto *negation = std::get_if<NotCondition>(&condition.value)) {
    if (!negation->child)
      throw std::logic_error("Cannot translate NOT without a child!");
    return "not (" + condition_text(*negation->child, aggregates) + ")";
  }
  const auto &logical = std::get<LogicalCondition>(condition.value);
  if (!logical.left || !logical.right)
    throw std::logic_error(
        "Cannot translate a condition with missing children!");
  std::string op;
  if (logical.op == LogicalOperator::AND)
    op = " and ";
  else if (logical.op == LogicalOperator::OR)
    op = " or ";
  else
    throw std::logic_error("Unknown logical operator during translation!");
  return "(" + condition_text(*logical.left, aggregates) + op +
         condition_text(*logical.right, aggregates) + ")";
}

std::string Translator::table_name(const TableRef &table) const {
  if (!table.output_schema)
    throw std::logic_error("Translation requires a resolved table reference!");
  if (const auto *named = std::get_if<NamedTable>(&table.value))
    return named->alias ? named->alias->name : named->name.name;
  return std::get<SubqueryTable>(table.value).alias.name;
}

void Translator::append_input_names(const TableRef &table,
                                    std::vector<std::string> &names) const {
  std::string qualifier = table_name(table);
  for (std::size_t i{0}; i < table.output_schema->size(); ++i)
    names.push_back(qualifier + "." + (*table.output_schema)[i].name);
}

std::size_t Translator::source_position(const ColumnRef &column,
                                        const FromClause &from) const {
  column_text(column);
  std::vector<const TableRef *> tables;
  if (const auto *comma = std::get_if<CommaFrom>(&from)) {
    for (const TableRef &table : comma->tables)
      tables.push_back(&table);
  } else {
    const auto &joined = std::get<JoinFrom>(from);
    tables.push_back(&joined.first);
    for (const JoinClause &join : joined.joins)
      tables.push_back(&join.right);
  }
  std::size_t offset = 0;
  for (const TableRef *table : tables) {
    if (table_name(*table) == column.qualifier->name) {
      std::size_t position = *column.resolved_position;
      if (position >= table->output_schema->size() ||
          (*table->output_schema)[position].name != column.name.name)
        throw std::logic_error(
            "Resolved column position does not match its table!");
      return offset + position;
    }
    offset += table->output_schema->size();
  }
  throw std::logic_error("Resolved column has no FROM scope entry!");
}

Json::Value Translator::translate_table(const TableRef &table) const {
  table_name(table);
  if (const auto *named = std::get_if<NamedTable>(&table.value)) {
    Json::Value scan(Json::objectValue);
    scan["op"] = "scan";
    scan["table"] = named->name.name;
    if (!named->alias)
      return scan;
    Json::Value rename = logical_unary("rename", std::move(scan));
    rename["to"] = named->alias->name;
    return rename;
  }
  const auto &subquery = std::get<SubqueryTable>(table.value);
  if (!subquery.query)
    throw std::logic_error("Cannot translate a missing subquery!");
  Json::Value rename = logical_unary("rename", translate(*subquery.query));
  rename["to"] = subquery.alias.name;
  return rename;
}

Json::Value Translator::translate_from(const FromClause &from) const {
  if (const auto *comma = std::get_if<CommaFrom>(&from)) {
    if (comma->tables.empty())
      throw std::logic_error("Cannot translate an empty FROM list!");
    Json::Value plan = translate_table(comma->tables[0]);
    for (std::size_t i{1}; i < comma->tables.size(); ++i) {
      Json::Value cross(Json::objectValue);
      cross["op"] = "cross";
      cross["left"] = std::move(plan);
      cross["right"] = translate_table(comma->tables[i]);
      plan = std::move(cross);
    }
    return plan;
  }
  const auto &joined = std::get<JoinFrom>(from);
  Json::Value plan = translate_table(joined.first);
  for (const JoinClause &join : joined.joins) {
    Json::Value node(Json::objectValue);
    node["op"] = "join";
    node["condition"] = condition_text(join.on, {});
    node["left"] = std::move(plan);
    node["right"] = translate_table(join.right);
    plan = std::move(node);
  }
  return plan;
}

Json::Value Translator::translate_select(const SelectBlock &select) const {
  const auto *items = std::get_if<std::vector<SelectItem>>(&select.columns);
  if (!items || items->empty())
    throw std::logic_error("Translation requires an expanded SELECT list!");

  std::vector<ColumnRef> keys;
  if (select.group_by)
    keys = select.group_by->columns;
  std::vector<AggregateBinding> aggregates;
  for (const SelectItem &item : *items)
    if (const auto *aggregate = std::get_if<AggregateCall>(&item.expression))
      add_aggregate(*aggregate, keys, aggregates);
  if (select.group_by && select.group_by->having)
    collect_having(*select.group_by->having, keys, aggregates);
  if ((!aggregates.empty() || select.group_by) && !select.aggregating)
    throw std::logic_error("Aggregate query was not marked during analysis!");

  Json::Value plan = translate_from(select.from);
  if (select.where) {
    Json::Value node = logical_unary("select", std::move(plan));
    node["condition"] = condition_text(*select.where, {});
    plan = std::move(node);
  }

  std::vector<std::string> input_names;
  if (select.aggregating) {
    Json::Value group = logical_unary("group_by", std::move(plan));
    group["keys"] = Json::Value(Json::arrayValue);
    group["aggregates"] = Json::Value(Json::arrayValue);
    for (const ColumnRef &key : keys) {
      std::string name = column_text(key);
      group["keys"].append(name);
      input_names.push_back(std::move(name));
    }
    for (const AggregateBinding &binding : aggregates) {
      Json::Value aggregate(Json::arrayValue);
      aggregate.append(aggregate_function(binding.aggregate->function));
      if (const auto *column =
              std::get_if<ColumnRef>(&binding.aggregate->argument))
        aggregate.append(column_text(*column));
      else
        aggregate.append("*");
      aggregate.append(binding.name);
      group["aggregates"].append(std::move(aggregate));
      input_names.push_back(binding.name);
    }
    plan = std::move(group);
    if (select.group_by && select.group_by->having) {
      Json::Value node = logical_unary("select", std::move(plan));
      node["condition"] = condition_text(*select.group_by->having, aggregates);
      plan = std::move(node);
    }
  } else if (const auto *comma = std::get_if<CommaFrom>(&select.from)) {
    for (const TableRef &table : comma->tables)
      append_input_names(table, input_names);
  } else {
    const auto &joined = std::get<JoinFrom>(select.from);
    append_input_names(joined.first, input_names);
    for (const JoinClause &join : joined.joins)
      append_input_names(join.right, input_names);
  }

  std::vector<std::string> sources;
  std::vector<std::string> output_names;
  std::vector<std::size_t> positions;
  for (const SelectItem &item : *items) {
    if (item.output_name.empty())
      throw std::logic_error("Selected item has no analyzed output name!");
    if (const auto *column = std::get_if<ColumnRef>(&item.expression)) {
      sources.push_back(column_text(*column));
      if (select.aggregating) {
        std::size_t position = 0;
        while (position < keys.size() &&
               !columns_match(*column, keys[position]))
          ++position;
        if (position == keys.size())
          throw std::logic_error("Projected column is not a grouping key!");
        positions.push_back(position);
      } else {
        positions.push_back(source_position(*column, select.from));
      }
      output_names.push_back(item.alias ? item.alias->name : sources.back());
    } else {
      const auto &aggregate = std::get<AggregateCall>(item.expression);
      std::size_t position = aggregate_position(aggregate, aggregates);
      sources.push_back(aggregates[position].name);
      positions.push_back(keys.size() + position);
      output_names.push_back(item.output_name);
    }
  }

  std::vector<std::string> hidden_names;
  std::vector<std::size_t> hidden_positions;
  std::vector<std::size_t> sort_positions;
  for (const SortKey &key : select.order_by) {
    if (key.output_position) {
      if (*key.output_position >= items->size())
        throw std::logic_error("ORDER BY output position is out of range!");
      sort_positions.push_back(*key.output_position);
      continue;
    }
    std::size_t selected = 0;
    for (; selected < items->size(); ++selected) {
      const auto *column =
          std::get_if<ColumnRef>(&(*items)[selected].expression);
      if (column && columns_match(*column, key.column))
        break;
    }
    if (selected < items->size()) {
      sort_positions.push_back(selected);
      continue;
    }
    if (select.distinct)
      throw std::logic_error(
          "Analysis allowed DISTINCT with a hidden sort column!");

    std::string name = column_text(key.column);
    std::size_t position;
    if (select.aggregating) {
      position = 0;
      while (position < keys.size() &&
             !columns_match(key.column, keys[position]))
        ++position;
      if (position == keys.size())
        throw std::logic_error("Hidden sort column is not a grouping key!");
    } else {
      position = source_position(key.column, select.from);
    }
    std::size_t hidden = 0;
    while (hidden < hidden_positions.size() &&
           hidden_positions[hidden] != position)
      ++hidden;
    if (hidden == hidden_positions.size()) {
      hidden_names.push_back(std::move(name));
      hidden_positions.push_back(position);
    }
    sort_positions.push_back(items->size() + hidden);
  }

  Json::Value project = logical_unary("project", std::move(plan));
  project["columns"] = Json::Value(Json::arrayValue);
  bool aliases = false;
  bool positional = false;
  for (std::size_t i{0}; i < sources.size(); ++i) {
    project["columns"].append(sources[i]);
    if (sources[i] != output_names[i])
      aliases = true;
    if (repeated_plan_name(input_names, sources[i]))
      positional = true;
  }
  if (aliases) {
    project["aliases"] = Json::Value(Json::arrayValue);
    for (const std::string &name : output_names)
      project["aliases"].append(name);
  }
  if (!hidden_names.empty()) {
    project["hidden_columns"] = Json::Value(Json::arrayValue);
    for (const std::string &name : hidden_names) {
      project["hidden_columns"].append(name);
      if (repeated_plan_name(input_names, name))
        positional = true;
    }
  }
  if (positional) {
    project["positions"] = Json::Value(Json::arrayValue);
    for (std::size_t position : positions)
      project["positions"].append(Json::UInt64(position));
    if (!hidden_positions.empty()) {
      project["hidden_positions"] = Json::Value(Json::arrayValue);
      for (std::size_t position : hidden_positions)
        project["hidden_positions"].append(Json::UInt64(position));
    }
  }
  plan = std::move(project);

  if (select.distinct && !select.aggregating)
    plan = logical_unary("distinct", std::move(plan));

  if (!select.order_by.empty()) {
    std::vector<std::string> projected_names = output_names;
    projected_names.insert(projected_names.end(), hidden_names.begin(),
                           hidden_names.end());
    Json::Value sort = logical_unary("sort", std::move(plan));
    sort["keys"] = Json::Value(Json::arrayValue);
    bool positional_keys = false;
    for (std::size_t i{0}; i < select.order_by.size(); ++i) {
      Json::Value key(Json::arrayValue);
      const std::string &name = projected_names[sort_positions[i]];
      key.append(name);
      if (select.order_by[i].direction == SortDirection::ASC)
        key.append("asc");
      else if (select.order_by[i].direction == SortDirection::DESC)
        key.append("desc");
      else
        throw std::logic_error("Unknown sort direction during translation!");
      sort["keys"].append(std::move(key));
      if (repeated_plan_name(projected_names, name))
        positional_keys = true;
    }
    if (positional_keys) {
      sort["key_positions"] = Json::Value(Json::arrayValue);
      for (std::size_t position : sort_positions)
        sort["key_positions"].append(Json::UInt64(position));
    }
    if (!hidden_names.empty())
      sort["drop_hidden"] = true;
    plan = std::move(sort);
  }
  if (select.limit) {
    if (select.limit->count < 0)
      throw std::logic_error("Analysis allowed a negative LIMIT!");
    Json::Value limit = logical_unary("limit", std::move(plan));
    limit["count"] = select.limit->count;
    plan = std::move(limit);
  }
  return plan;
}

Json::Value Translator::translate(const Query &query) const {
  if (!query.output_schema)
    throw std::logic_error("Run semantic analysis before translation!");
  if (const auto *select = std::get_if<SelectBlock>(&query.value))
    return translate_select(*select);

  const auto &operation = std::get<SetOperation>(query.value);
  if (!operation.left || !operation.right)
    throw std::logic_error(
        "Cannot translate a set operation with missing children!");
  Json::Value node(Json::objectValue);
  switch (operation.op) {
  case SetOperator::UNION:
    node["op"] = "union";
    break;
  case SetOperator::UNION_ALL:
    node["op"] = "union_all";
    break;
  case SetOperator::INTERSECT:
    node["op"] = "intersect";
    break;
  case SetOperator::EXCEPT:
    node["op"] = "except";
    break;
  default:
    throw std::logic_error("Unknown set operator during translation!");
  }
  node["left"] = translate(*operation.left);
  node["right"] = translate(*operation.right);
  return node;
}

} // namespace vql::query
