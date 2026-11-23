#include "algebra.h"
#include "diagnostic.h"
#include <unordered_map>
#include <utility>

static std::string qualified_column_name(const Relation &relation,
                                         const Column &column) {
  if (column.name.find('.') != std::string::npos)
    return column.name;

  return relation.name() + "." + column.name;
}

static void append_qualified_columns(
    const Relation &relation, std::vector<Column> &columns,
    std::unordered_map<std::string, int> &names) {
  for (const Column &column : relation.schema().columns()) {
    std::string name = qualified_column_name(relation, column);

    if (names.contains(name))
      throw DiagnosticError(DiagnosticCategory::Schema,
                            "DUPLICATE COLUMNS IN TIMES");

    names[name] = 1;
    columns.push_back({.name = std::move(name), .type = column.type});
  }
}

Relation Algebra::selection(const Relation &relation, Predicate predicate,
                            OperationStats *stats) {
  Relation out(relation.name(), relation.schema());

  for (const auto &tuple : relation.tuples()) {
    if (stats != nullptr) {
      ++stats->selection_examinations;
    }

    if (predicate(tuple, relation.schema(), relation.name()))
      out.insert_row(tuple);
  }

  return out;
}

Relation Algebra::projection(const Relation &relation,
                             const std::vector<std::string> &attributes) {
  const Schema &schema = relation.schema();
  const auto &old_columns = schema.columns();
  std::vector<std::size_t> indexes;
  std::vector<Column> new_columns;
  indexes.reserve(attributes.size());
  new_columns.reserve(attributes.size());

  std::unordered_map<std::string, int> checker;

  for (const std::string &attribute : attributes) {
    const std::size_t index = schema.index_of(attribute);
    if (index == Schema::npos)
      throw DiagnosticError(DiagnosticCategory::Name, "UNKNOWN ATTRIBUTE");

    if (checker.contains(attribute)) {
      continue;
    }

    checker[attribute] = 1;

    indexes.push_back(index);
    new_columns.push_back(old_columns[index]);
  }

  Relation out(relation.name(), Schema(std::move(new_columns)));

  for (const auto &tuple : relation.tuples()) {
    Tuple new_tuple;
    new_tuple.reserve(indexes.size());
    for (const std::size_t index : indexes)
      new_tuple.push_back(tuple[index]);

    out.insert_row(new_tuple);
  }

  return out;
}

Relation Algebra::rename(const Relation &relation, std::string new_name) {
  Relation out(std::move(new_name), relation.schema());

  for (const auto &tuple : relation.tuples())
    out.insert_row(tuple);

  return out;
}

Relation Algebra::rename(const Relation &relation, std::string old_attr,
                         std::string new_attr) {
  const Schema &schema = relation.schema();
  const std::size_t index = schema.index_of(old_attr);

  if (index == Schema::npos)
    throw DiagnosticError(DiagnosticCategory::Name,
                          "UNKNOWN ATTRIBUTE: " + old_attr);

  const std::size_t existing_index = schema.index_of(new_attr);
  if (existing_index != Schema::npos && existing_index != index)
    throw DiagnosticError(DiagnosticCategory::Schema,
                          "ATTRIBUTE ALREADY EXISTS: " + new_attr);

  std::vector<Column> new_columns = schema.columns();
  new_columns[index] = {.name = std::move(new_attr),
                        .type = schema.columns()[index].type};

  Relation out(relation.name(), Schema(std::move(new_columns)));

  for (const auto &tuple : relation.tuples())
    out.insert_row(tuple);

  return out;
}

Relation Algebra::times(const Relation &rel_1, const Relation &rel_2,
                        std::string modifier) {
  std::vector<Column> columns;
  std::unordered_map<std::string, int> checker;

  append_qualified_columns(rel_1, columns, checker);
  append_qualified_columns(rel_2, columns, checker);

  Relation out(rel_1.name() + " " + modifier + " " + rel_2.name(),
               Schema(std::move(columns)));

  for (const auto &tuple_1 : rel_1.tuples())
    for (const auto &tuple_2 : rel_2.tuples()) {
      Tuple new_tuple;
      new_tuple.reserve(tuple_1.size() + tuple_2.size());
      new_tuple.insert(new_tuple.end(), tuple_1.begin(), tuple_1.end());
      new_tuple.insert(new_tuple.end(), tuple_2.begin(), tuple_2.end());
      out.insert_row(new_tuple);
    }

  return out;
}

Relation Algebra::join(const Relation &rel_1, const Relation &rel_2,
                       Predicate predicate, OperationStats *stats,
                       ConditionValidator validator) {
  std::vector<Column> columns;
  columns.reserve(rel_1.schema().columns().size() +
                  rel_2.schema().columns().size());

  std::unordered_map<std::string, int> checker;

  append_qualified_columns(rel_1, columns, checker);
  append_qualified_columns(rel_2, columns, checker);

  Relation output(rel_1.name() + " JOIN " + rel_2.name(),
                  Schema(std::move(columns)));

  if (validator)
    validator(output.schema(), output.name());

  for (const Tuple &left_tuple : rel_1.tuples()) {
    for (const Tuple &right_tuple : rel_2.tuples()) {
      if (stats != nullptr)
        ++stats->join_comparisons;

      Tuple combined;
      combined.reserve(left_tuple.size() + right_tuple.size());
      combined.insert(combined.end(), left_tuple.begin(), left_tuple.end());
      combined.insert(combined.end(), right_tuple.begin(), right_tuple.end());

      if (predicate(combined, output.schema(), output.name()))
        output.insert_row(combined);
    }
  }

  return output;
}

Relation Algebra::intersect(const Relation &rel_1, const Relation &rel_2) {
  if (!compare_schemas(rel_1, rel_2))
    throw DiagnosticError(DiagnosticCategory::Schema,
                          "SCHEMA'S NOT COMPATIBLE");

  Relation out(rel_1.name(), rel_1.schema());

  for (const auto &tuple : rel_1.tuples())
    if (rel_2.contains(tuple))
      out.insert_row(tuple);

  return out;
}

Relation Algebra::minus(const Relation &rel_1, const Relation &rel_2) {
  if (!compare_schemas(rel_1, rel_2))
    throw DiagnosticError(DiagnosticCategory::Schema,
                          "SCHEMA'S NOT COMPATIBLE");

  Relation out(rel_1.name(), rel_1.schema());

  for (const auto &tuple : rel_1.tuples())
    if (!rel_2.contains(tuple))
      out.insert_row(tuple);

  return out;
}

Relation Algebra::onion(const Relation &rel_1, const Relation &rel_2) {
  if (!compare_schemas(rel_1, rel_2))
    throw DiagnosticError(DiagnosticCategory::Schema,
                          "SCHEMA'S NOT COMPATIBLE");

  Relation out(rel_1.name(), rel_1.schema());

  for (const auto &tuple : rel_1.tuples())
    out.insert_row(tuple);

  for (const auto &tuple : rel_2.tuples())
    out.insert_row(tuple);

  return out;
}

bool Algebra::compare_schemas(const Relation &rel_1, const Relation &rel_2) {
  const Schema &schema_1 = rel_1.schema();
  const Schema &schema_2 = rel_2.schema();

  if (schema_1.columns().size() != schema_2.columns().size())
    return false;

  for (std::size_t i{}; i < schema_1.columns().size(); ++i)
    if (schema_1.columns()[i].name != schema_2.columns()[i].name ||
        schema_1.columns()[i].type != schema_2.columns()[i].type)
      return false;

  return true;
}
