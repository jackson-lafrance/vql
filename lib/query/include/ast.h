#ifndef VQL_QUERY_AST_H
#define VQL_QUERY_AST_H

#include "source.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace vql::query {

struct NameWithSpan {
  std::string name;
  Span span;
};

struct ColumnRef {
  std::optional<NameWithSpan> qualifier;
  NameWithSpan name;
  Span span;
};

struct Literal {
  std::variant<std::int32_t, std::string> value;
  Span span;
};

enum class AggregateFunction { COUNT, SUM, AVG, MIN, MAX };

struct Star {
  Span span;
};

struct AggregateCall {
  AggregateFunction function;
  std::variant<ColumnRef, Star> argument;
  Span span;
};

using Operand = std::variant<ColumnRef, Literal, AggregateCall>;

enum class ComparisonOperator {
  EQUAL,
  NOT_EQUAL,
  LESS_THAN,
  LESS_THAN_OR_EQUAL,
  GREATER_THAN,
  GREATER_THAN_OR_EQUAL
};

struct Comparison {
  Operand left;
  ComparisonOperator op;
  Operand right;
  Span operator_span;
};

struct Condition;

struct NotCondition {
  std::unique_ptr<Condition> child;
  Span operator_span;
};

enum class LogicalOperator { AND, OR };

struct LogicalCondition {
  LogicalOperator op;
  std::unique_ptr<Condition> left;
  std::unique_ptr<Condition> right;
  Span operator_span;
};

struct Condition {
  std::variant<Comparison, NotCondition, LogicalCondition> value;
  Span span;
};

struct SelectItem {
  std::variant<ColumnRef, AggregateCall> expression;
  std::optional<NameWithSpan> alias;
  Span span;
};

using SelectList = std::variant<Star, std::vector<SelectItem>>;

struct Query;

struct NamedTable {
  NameWithSpan name;
  std::optional<NameWithSpan> alias;
};

struct SubqueryTable {
  std::unique_ptr<Query> query;
  NameWithSpan alias;
};

struct TableRef {
  std::variant<NamedTable, SubqueryTable> value;
  Span span;
};

struct CommaFrom {
  std::vector<TableRef> tables;
};

struct JoinClause {
  TableRef right;
  Condition on;
  Span span;
};

struct JoinFrom {
  TableRef first;
  std::vector<JoinClause> joins;
};

using FromClause = std::variant<CommaFrom, JoinFrom>;

struct GroupByClause {
  std::vector<ColumnRef> columns;
  std::optional<Condition> having;
  Span span;
};

enum class SortDirection { ASC, DESC };

struct SortKey {
  ColumnRef column;
  SortDirection direction{SortDirection::ASC};
  Span span;
};

struct LimitClause {
  std::int32_t count;
  Span span;
};

struct SelectBlock {
  bool distinct{false};
  SelectList columns;
  FromClause from;
  std::optional<Condition> where;
  std::optional<GroupByClause> group_by;
  std::vector<SortKey> order_by;
  std::optional<LimitClause> limit;
  Span span;
};

enum class SetOperator { UNION, UNION_ALL, INTERSECT, EXCEPT };

struct SetOperation {
  SetOperator op;
  std::unique_ptr<Query> left;
  std::unique_ptr<Query> right;
  Span operator_span; // Includes both keywords for UNION ALL.
  Span span;
};

struct Query {
  std::variant<SelectBlock, SetOperation> value;
  Span span;
};

struct IntType {};

struct VarcharType {
  std::int32_t length;
};

struct DataType {
  std::variant<IntType, VarcharType> value;
  Span span;
};

struct ColumnDefinition {
  NameWithSpan name;
  DataType type;
  Span span;
};

struct CreateTable {
  NameWithSpan table;
  std::vector<ColumnDefinition> columns;
  Span span;
};

struct Insert {
  NameWithSpan table;
  std::vector<Literal> values;
  Span span;
};

using Statement = std::variant<CreateTable, Insert, Query>;

struct Script {
  std::vector<Statement> statements;
};

} // namespace vql::query

#endif // VQL_QUERY_AST_H
