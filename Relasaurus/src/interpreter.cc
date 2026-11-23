#include "interpreter.h"
#include "algebra.h"
#include "diagnostic.h"
#include "parser.h"
#include "relation.h"
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

static std::string resolve_unqualified_attribute(const std::string &name,
                                                 const Schema &schema) {
  const Column *match = nullptr;
  const std::string qualified_suffix = "." + name;

  for (const Column &column : schema.columns()) {
    if (column.name != name && !column.name.ends_with(qualified_suffix))
      continue;

    if (match != nullptr)
      throw DiagnosticError(DiagnosticCategory::Name,
                            "AMBIGUOUS ATTRIBUTE: " + name);

    match = &column;
  }

  if (match == nullptr)
    throw DiagnosticError(DiagnosticCategory::Name,
                          "UNKNOWN ATTRIBUTE: " + name);

  return match->name;
}

static std::string resolve_attribute(const AttributeReference &reference,
                                     const Schema &schema,
                                     const std::string &relation_name) {
  const std::string &attribute_name = reference.attribute_name.name;

  if (!reference.relation_name.has_value())
    return resolve_unqualified_attribute(attribute_name, schema);

  const std::string &qualifier = reference.relation_name->name;
  const std::string qualified_name = qualifier + "." + attribute_name;

  if (schema.index_of(qualified_name) != Schema::npos)
    return qualified_name;

  if (qualifier == relation_name)
    return resolve_unqualified_attribute(attribute_name, schema);

  throw DiagnosticError(DiagnosticCategory::Name,
                        "UNKNOWN QUALIFIED ATTRIBUTE: " + qualified_name);
}

static Type validate_operand(const Operand &operand, const Schema &schema,
                             const std::string &relation_name) {
  if (std::holds_alternative<double>(operand.value))
    return NUMBER;

  if (std::holds_alternative<std::string>(operand.value))
    return STRING;

  if (const auto *ref = std::get_if<AttributeReference>(&operand.value)) {
    const std::string name = resolve_attribute(*ref, schema, relation_name);
    return schema.columns()[schema.index_of(name)].type;
  }

  throw std::logic_error("UNKNOWN OPERAND TYPE");
}

static void validate_condition(const Condition &condition, const Schema &schema,
                               const std::string &relation_name) {
  if (const auto *comparison =
          std::get_if<ComparisonCondition>(&condition.node)) {
    const Type left = validate_operand(comparison->left, schema, relation_name);
    const Type right =
        validate_operand(comparison->right, schema, relation_name);

    if (left != right) {
      throw DiagnosticError(DiagnosticCategory::Type,
                            std::string("CANNOT COMPARE ") +
                                (left == NUMBER ? "NUMBER" : "STRING") +
                                " TO A " +
                                (right == NUMBER ? "NUMBER" : "STRING"));
    }
    return;
  }

  if (const auto *logical = std::get_if<LogicalCondition>(&condition.node)) {
    validate_condition(*logical->left, schema, relation_name);
    validate_condition(*logical->right, schema, relation_name);
    return;
  }

  if (const auto *negation = std::get_if<NotCondition>(&condition.node)) {
    validate_condition(*negation->operand, schema, relation_name);
    return;
  }

  throw std::logic_error("UNKNOWN CONDITION TYPE!!");
}

const OperationStats &Interpreter::stats() const { return stats_; }

std::optional<Relation> Interpreter::execute(const Statement &statement) {
  if (const auto *definition =
          std::get_if<RelationDefinition>(&statement.guy)) {
    const std::string &name = definition->name.name;

    if (relations_.contains(name)) {
      throw DiagnosticError(DiagnosticCategory::Name,
                            "RELATION ALREADY EXISTS: " + name);
    }

    std::vector<Column> columns;
    columns.reserve(definition->columns.size());

    for (const ColumnWithSpan &column : definition->columns) {
      columns.push_back(column.column);
    }

    Relation relation(name, Schema(std::move(columns)));

    for (const TupleWithSpan &tuple : definition->tuples) {
      relation.insert_row(tuple.tuple);
    }

    relations_.emplace(name, std::move(relation));
    return std::nullopt;
  }

  if (const auto *insertion = std::get_if<RelationInsertion>(&statement.guy)) {
    auto existing = relations_.find(insertion->name.name);

    if (existing == relations_.end()) {
      throw DiagnosticError(DiagnosticCategory::Name,
                            "RELATION DOES NOT EXIST: " +
                                insertion->name.name);
    }

    Relation updated = existing->second;

    for (const TupleWithSpan &tuple : insertion->tuples) {
      updated.insert_row(tuple.tuple);
    }

    existing->second = std::move(updated);
    return std::nullopt;
  }

  if (const auto *query = std::get_if<REX>(&statement.guy)) {
    return evaluate(*query);
  }

  throw std::logic_error("UNKNOWN STATEMENT TYPE");
}

Relation Interpreter::evaluate(const REX &expression) {
  if (const auto *name = std::get_if<NameWithSpan>(&expression.node)) {
    const auto existing = relations_.find(name->name);

    if (existing == relations_.end()) {
      throw DiagnosticError(DiagnosticCategory::Name,
                            "RELATION DOES NOT EXIST: " + name->name);
    }

    return existing->second;
  }

  if (const auto *binary = std::get_if<BinaryExpression>(&expression.node)) {
    Relation left = evaluate(*binary->left);
    Relation right = evaluate(*binary->right);

    switch (binary->operation) {
    case BinaryOperator::UNION:
      return Algebra::onion(left, right);

    case BinaryOperator::INTERSECT:
      return Algebra::intersect(left, right);

    case BinaryOperator::MINUS:
      return Algebra::minus(left, right);

    case BinaryOperator::TIMES:
      return Algebra::times(left, right);
    }

    throw std::logic_error("UNKNOWN BINARY OPERATOR");
  }

  if (const auto *join = std::get_if<JoinExpression>(&expression.node)) {
    Relation left = evaluate(*join->left);
    Relation right = evaluate(*join->right);

    return Algebra::join(
        left, right,
        [this, join](const Tuple &tuple, const Schema &schema,
                     const std::string &relation_name) {
          return eval_cond(join->condition, tuple, schema, relation_name);
        },
        &stats_,
        [join](const Schema &schema, const std::string &relation_name) {
          validate_condition(join->condition, schema, relation_name);
        });
  }

  if (const auto *selection = std::get_if<SelectExpression>(&expression.node)) {
    Relation input = evaluate(*selection->input);
    validate_condition(selection->condition, input.schema(), input.name());

    return Algebra::selection(
        input,
        [this, selection](const Tuple &tuple, const Schema &schema,
                          const std::string &relation_name) {
          return eval_cond(selection->condition, tuple, schema, relation_name);
        },
        &stats_);
  }

  if (const auto *projection =
          std::get_if<ProjectExpression>(&expression.node)) {
    Relation input = evaluate(*projection->input);

    std::vector<std::string> attrs;
    attrs.reserve(projection->attributes.size());

    for (const AttributeReference &ref : projection->attributes) {
      attrs.push_back(resolve_attribute(ref, input.schema(), input.name()));
    }

    return Algebra::projection(input, attrs);
  }

  if (const auto *relation_rename =
          std::get_if<RenameTableExpression>(&expression.node)) {
    Relation input = evaluate(*relation_rename->input);

    return Algebra::rename(input, relation_rename->new_name.name);
  }

  if (const auto *attr_rename =
          std::get_if<RenameAttributeExpression>(&expression.node)) {
    Relation input = evaluate(*attr_rename->input);
    const std::string name = resolve_attribute(
        attr_rename->old_name, input.schema(), input.name());

    return Algebra::rename(input, name, attr_rename->new_name.name);
  }

  throw std::logic_error("UNKNOWN RELATIONAL EXPRESSION TYPE!!");
}

bool Interpreter::eval_cond(const Condition &condition, const Tuple &tuple,
                            const Schema &schema,
                            const std::string &relation_name) {
  if (const auto *comparison_condition =
          std::get_if<ComparisonCondition>(&condition.node)) {
    const Value left =
        eval_oppa(comparison_condition->left, tuple, schema, relation_name);
    const Value right =
        eval_oppa(comparison_condition->right, tuple, schema, relation_name);

    if (left.index() != right.index()) {
      throw DiagnosticError(
          DiagnosticCategory::Type,
          std::string("CANNOT COMPARE ") +
              (std::holds_alternative<double>(left) ? "NUMBER" : "STRING") +
              " TO A " +
              (std::holds_alternative<double>(right) ? "NUMBER" : "STRING"));
    }

    switch (comparison_condition->lil_durk) {
    case ComparisonOperator::Equal:
      return left == right;
    case ComparisonOperator::NotEqual:
      return left != right;
    case ComparisonOperator::Less:
      return left < right;
    case ComparisonOperator::LessEqual:
      return left <= right;
    case ComparisonOperator::Greater:
      return left > right;
    case ComparisonOperator::GreaterEqual:
      return left >= right;
    default:
      throw std::logic_error("UNKNOWN COMPARISON OPERATOR!!");
    }
  }

  if (const auto *logical_condition =
          std::get_if<LogicalCondition>(&condition.node)) {
    if (logical_condition->king_von == LogicalOperator::And) {
      return eval_cond(*logical_condition->left, tuple, schema,
                       relation_name) &&
             eval_cond(*logical_condition->right, tuple, schema, relation_name);
    } else if (logical_condition->king_von == LogicalOperator::Or) {
      return eval_cond(*logical_condition->left, tuple, schema,
                       relation_name) ||
             eval_cond(*logical_condition->right, tuple, schema, relation_name);
    }

    throw std::logic_error("UNKNOWN LOGIC OPERATOR!!");
  }

  if (const auto *not_condition = std::get_if<NotCondition>(&condition.node)) {
    return !eval_cond(*not_condition->operand, tuple, schema, relation_name);
  }

  throw std::logic_error("UNKNOWN CONDITION TYPE!!");
}

Value Interpreter::eval_oppa(const Operand &operand, const Tuple &tuple,
                             const Schema &schema,
                             const std::string &relation_name) {
  if (const auto *number = std::get_if<double>(&operand.value)) {
    return *number;
  }

  if (const auto *string = std::get_if<std::string>(&operand.value)) {
    return *string;
  }

  if (const auto *ref = std::get_if<AttributeReference>(&operand.value)) {
    const std::string name = resolve_attribute(*ref, schema, relation_name);
    return tuple[schema.index_of(name)];
  }

  throw std::logic_error("UNKNOWN OPERAND TYPE");
}
