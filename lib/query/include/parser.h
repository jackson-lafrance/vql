#ifndef VQL_QUERY_PARSER_H
#define VQL_QUERY_PARSER_H

#include "ast.h"
#include "lexer.h"

namespace vql::query {

class Parser {
public:
  static Script parse(const std::string &source);
  static Script parse_tokens(const std::vector<Token> &tokens);

  static Condition parse_condition(const std::string &source);
  static Condition parse_condition_tokens(const std::vector<Token> &tokens);

private:
  explicit Parser(const std::vector<Token> &tokens);

  const Token &peek() const;
  const Token &prev() const;
  const Token &advance();
  bool at_end() const;
  bool check(TokenType type) const;
  bool match(TokenType type);
  const Token &consume(TokenType type, const std::string &message);
  [[noreturn]] void fail(const std::string &message) const;

  Statement parse_statement();
  CreateTable parse_create();
  ColumnDefinition parse_column_definition();
  Insert parse_insert();
  Literal parse_literal();
  NameWithSpan parse_name();

  Query parse_query();
  Query parse_query_primary();
  SelectBlock parse_select();
  SelectList parse_select_list();
  SelectItem parse_select_item();
  AggregateCall parse_aggregate();
  FromClause parse_from();
  TableRef parse_table();
  GroupByClause parse_group_by();
  SortKey parse_sort_key();
  ColumnRef parse_column();

  Condition parse_or();
  Condition parse_and();
  Condition parse_not();
  Condition parse_comparison();
  Operand parse_operand();
  bool at_aggregate() const;

  const std::vector<Token> &tokens_;
  std::size_t current_{0};
};

} // namespace vql::query

#endif // VQL_QUERY_PARSER_H
