#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "relation.h"
#include "schema.h"
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

struct REX;
struct Condition;

struct NameWithSpan {
  std::string name;
  Span span;
};

struct TupleWithSpan {
  Tuple tuple;
  Span span;
};

struct ColumnWithSpan {
  Column column;
  Span span;
};

struct RelationDefinition {
  NameWithSpan name;
  std::vector<ColumnWithSpan> columns;
  std::vector<TupleWithSpan> tuples;
};

struct RelationInsertion {
  NameWithSpan name;
  std::vector<TupleWithSpan> tuples;
};

enum class BinaryOperator { UNION, INTERSECT, MINUS, TIMES };

struct AttributeReference {
  std::optional<NameWithSpan> relation_name;
  NameWithSpan attribute_name;
  Span span;
};

struct Operand {
  std::variant<double, std::string, AttributeReference> value;
  Span span;
};

enum class ComparisonOperator {
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
};

struct ComparisonCondition {
  Operand left;
  Operand right;
  ComparisonOperator lil_durk;
};

enum class LogicalOperator {
  And,
  Or,
};

struct LogicalCondition {
  std::unique_ptr<Condition> left;
  std::unique_ptr<Condition> right;
  LogicalOperator king_von;
};

struct NotCondition {
  std::unique_ptr<Condition> operand;
};

struct Condition {
  std::variant<ComparisonCondition, LogicalCondition, NotCondition> node;
  Span span;
};

struct JoinExpression {
  Condition condition;
  std::unique_ptr<REX> left;
  std::unique_ptr<REX> right;
};

struct BinaryExpression {
  BinaryOperator operation;
  std::unique_ptr<REX> left;
  std::unique_ptr<REX> right;
};

struct SelectExpression {
  Condition condition;
  std::unique_ptr<REX> input;
};

struct ProjectExpression {
  std::vector<AttributeReference> attributes;
  std::unique_ptr<REX> input;
};

struct RenameTableExpression {
  NameWithSpan new_name;
  std::unique_ptr<REX> input;
};

struct RenameAttributeExpression {
  NameWithSpan new_name;
  AttributeReference old_name;
  std::unique_ptr<REX> input;
};

struct REX {
  std::variant<NameWithSpan, BinaryExpression, JoinExpression, SelectExpression,
               ProjectExpression, RenameTableExpression,
               RenameAttributeExpression>
      node;
  Span span;
};

struct Statement {
  std::variant<RelationDefinition, RelationInsertion, REX> guy;
  Span span;
};

struct ParseError {
  std::string message;
  Span span;
  TType actual;
  std::optional<TType> expected;
};

class Parser {
public:
  static std::variant<Statement, ParseError>
  parse_tokens(std::vector<Token> &tokens);

private:
  explicit Parser(std::vector<Token> &tokens);

  const Token &peek() const;
  const Token &prev() const;
  const Token &advance();

  bool at_end() const;
  bool check(TType type) const;
  bool match(TType);

  const Token &consume(TType type, std::string error);

  Statement parse_statement();

  Statement parse_relation_definition();
  Statement parse_relation_insertion();
  Statement parse_query_statement();

  REX parse_rex();
  REX parse_primary();
  REX parse_select();
  REX parse_project();
  REX parse_rename_table();
  REX parse_rename_attribute();

  Condition parse_or();
  Condition parse_and();
  Condition parse_not();
  Condition parse_comparison();

  Operand parse_operand();
  AttributeReference parse_attribute_reference();

  std::vector<ColumnWithSpan> parse_schema();
  ColumnWithSpan parse_column();
  TupleWithSpan parse_tuple();
  Value parse_value();

  [[noreturn]] void fail(const Token &token, std::string message);
  [[noreturn]] void fail(const Token &token, TType expected,
                         std::string message);

  const std::vector<Token> &tokens_;
  std::size_t current_{0};
};

#endif // PARSER_H
