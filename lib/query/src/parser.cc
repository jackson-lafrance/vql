#include "parser.h"
#include "diagnostic.h"

#include <utility>

namespace vql::query {

Script Parser::parse(const std::string &source) {
  return parse_tokens(Lexer::tokenize(source));
}

Script Parser::parse_tokens(const std::vector<Token> &tokens) {
  Parser parser(tokens);
  Script script;
  while (!parser.at_end()) {
    script.statements.push_back(parser.parse_statement());
    parser.consume(TokenType::Semicolon, "Expected ';' after statement");
  }
  return script;
}

Condition Parser::parse_condition(const std::string &source) {
  return parse_condition_tokens(Lexer::tokenize(source));
}

Condition Parser::parse_condition_tokens(const std::vector<Token> &tokens) {
  Parser parser(tokens);
  Condition condition = parser.parse_or();
  parser.consume(TokenType::EndOfInput, "Expected end of condition");
  return condition;
}

Parser::Parser(const std::vector<Token> &tokens) : tokens_(tokens) {
  if (tokens.empty() || tokens.back().type != TokenType::EndOfInput)
    throw DiagnosticError(ErrorCategory::SYNTAX,
                          "Token input must end with EndOfInput", {});
}

const Token &Parser::peek() const { return tokens_[current_]; }
const Token &Parser::prev() const { return tokens_[current_ - 1]; }
const Token &Parser::advance() {
  const Token &token = peek();
  if (!at_end())
    ++current_;
  return token;
}
bool Parser::at_end() const { return check(TokenType::EndOfInput); }
bool Parser::check(TokenType type) const { return peek().type == type; }
bool Parser::match(TokenType type) {
  if (!check(type))
    return false;
  advance();
  return true;
}
const Token &Parser::consume(TokenType type, const std::string &message) {
  if (!check(type))
    fail(message);
  return advance();
}
[[noreturn]] void Parser::fail(const std::string &message) const {
  throw DiagnosticError(ErrorCategory::SYNTAX, message, peek().span);
}

Statement Parser::parse_statement() {
  if (check(TokenType::Create))
    return parse_create();
  if (check(TokenType::Insert))
    return parse_insert();
  return parse_query();
}

NameWithSpan Parser::parse_name() {
  const Token &name = consume(TokenType::Identifier, "Expected identifier");
  return {.name = name.lexeme, .span = name.span};
}

CreateTable Parser::parse_create() {
  Position begin = consume(TokenType::Create, "Expected CREATE").span.begin;
  consume(TokenType::Table, "Expected TABLE after CREATE");
  NameWithSpan table = parse_name();
  consume(TokenType::LeftParen, "Expected '(' before column definitions");
  std::vector<ColumnDefinition> columns;
  columns.push_back(parse_column_definition());
  while (match(TokenType::Comma))
    columns.push_back(parse_column_definition());
  Position end =
      consume(TokenType::RightParen, "Expected ')' after column definitions")
          .span.end;
  return {.table = std::move(table),
          .columns = std::move(columns),
          .span = {begin, end},
          .resolved_schema = std::nullopt};
}

ColumnDefinition Parser::parse_column_definition() {
  NameWithSpan name = parse_name();
  Position begin = name.span.begin;
  Position type_begin = peek().span.begin;
  DataType type;
  if (match(TokenType::Int)) {
    type = {.value = IntType{}, .span = prev().span};
  } else if (match(TokenType::Varchar)) {
    consume(TokenType::LeftParen, "Expected '(' after VARCHAR");
    const Token &length =
        consume(TokenType::Integer, "Expected VARCHAR length");
    Position end =
        consume(TokenType::RightParen, "Expected ')' after VARCHAR length")
            .span.end;
    type = {.value = VarcharType{std::get<std::int32_t>(length.value)},
            .span = {type_begin, end}};
  } else {
    fail("Expected INT or VARCHAR column type");
  }
  Span span{begin, type.span.end};
  return {.name = std::move(name), .type = std::move(type), .span = span};
}

Insert Parser::parse_insert() {
  Position begin = consume(TokenType::Insert, "Expected INSERT").span.begin;
  consume(TokenType::Into, "Expected INTO after INSERT");
  NameWithSpan table = parse_name();
  consume(TokenType::Values, "Expected VALUES after table name");
  consume(TokenType::LeftParen, "Expected '(' before values");
  std::vector<Literal> values;
  values.push_back(parse_literal());
  while (match(TokenType::Comma))
    values.push_back(parse_literal());
  Position end =
      consume(TokenType::RightParen, "Expected ')' after values").span.end;
  return {.table = std::move(table),
          .values = std::move(values),
          .span = {begin, end}};
}

Literal Parser::parse_literal() {
  if (match(TokenType::Integer))
    return {.value = std::get<std::int32_t>(prev().value), .span = prev().span};
  if (match(TokenType::String))
    return {.value = std::get<std::string>(prev().value), .span = prev().span};
  fail("Expected integer or string literal");
}

Query Parser::parse_query() {
  Query left = parse_query_primary();
  while (true) {
    SetOperator op;
    Span operator_span = peek().span;
    if (match(TokenType::Union)) {
      op = SetOperator::UNION;
      if (match(TokenType::All)) {
        op = SetOperator::UNION_ALL;
        operator_span.end = prev().span.end;
      }
    } else if (match(TokenType::Intersect)) {
      op = SetOperator::INTERSECT;
    } else if (match(TokenType::Except)) {
      op = SetOperator::EXCEPT;
    } else {
      break;
    }
    Query right = parse_query_primary();
    Span span{left.span.begin, right.span.end};
    SetOperation operation{.op = op,
                           .left = std::make_unique<Query>(std::move(left)),
                           .right = std::make_unique<Query>(std::move(right)),
                           .operator_span = operator_span,
                           .span = span};
    left = {.value = std::move(operation),
            .span = span,
            .output_schema = std::nullopt};
  }
  return left;
}

Query Parser::parse_query_primary() {
  if (match(TokenType::LeftParen)) {
    Position begin = prev().span.begin;
    Query query = parse_query();
    Position end =
        consume(TokenType::RightParen, "Expected ')' after query").span.end;
    query.span = {begin, end};
    return query;
  }
  SelectBlock select = parse_select();
  Span span = select.span;
  return {
      .value = std::move(select), .span = span, .output_schema = std::nullopt};
}

SelectBlock Parser::parse_select() {
  Position begin =
      consume(TokenType::Select, "Expected SELECT, CREATE, or INSERT")
          .span.begin;
  SelectBlock select;
  select.distinct = match(TokenType::Distinct);
  select.columns = parse_select_list();
  consume(TokenType::From, "Expected FROM after select list");
  select.from = parse_from();
  if (match(TokenType::Where))
    select.where = parse_or();
  if (check(TokenType::Group))
    select.group_by = parse_group_by();
  if (match(TokenType::Order)) {
    consume(TokenType::By, "Expected BY after ORDER");
    select.order_by.push_back(parse_sort_key());
    while (match(TokenType::Comma))
      select.order_by.push_back(parse_sort_key());
  }
  if (match(TokenType::Limit)) {
    Position limit_begin = prev().span.begin;
    const Token &count =
        consume(TokenType::Integer, "Expected integer after LIMIT");
    select.limit = LimitClause{.count = std::get<std::int32_t>(count.value),
                               .span = {limit_begin, count.span.end}};
  }
  select.span = {begin, prev().span.end};
  return select;
}

SelectList Parser::parse_select_list() {
  if (match(TokenType::Star))
    return Star{prev().span};
  std::vector<SelectItem> items;
  items.push_back(parse_select_item());
  while (match(TokenType::Comma))
    items.push_back(parse_select_item());
  return items;
}

SelectItem Parser::parse_select_item() {
  Position begin = peek().span.begin;
  SelectItem item;
  if (at_aggregate())
    item.expression = parse_aggregate();
  else
    item.expression = parse_column();
  if (match(TokenType::As))
    item.alias = parse_name();
  item.span = {begin, prev().span.end};
  return item;
}

bool Parser::at_aggregate() const {
  return check(TokenType::Count) || check(TokenType::Sum) ||
         check(TokenType::Avg) || check(TokenType::Min) ||
         check(TokenType::Max);
}

AggregateCall Parser::parse_aggregate() {
  Position begin = peek().span.begin;
  AggregateFunction function;
  if (match(TokenType::Count))
    function = AggregateFunction::COUNT;
  else if (match(TokenType::Sum))
    function = AggregateFunction::SUM;
  else if (match(TokenType::Avg))
    function = AggregateFunction::AVG;
  else if (match(TokenType::Min))
    function = AggregateFunction::MIN;
  else if (match(TokenType::Max))
    function = AggregateFunction::MAX;
  else
    fail("Expected aggregate function");
  consume(TokenType::LeftParen, "Expected '(' after aggregate function");
  std::variant<ColumnRef, Star> argument;
  if (match(TokenType::Star))
    argument = Star{prev().span};
  else
    argument = parse_column();
  Position end =
      consume(TokenType::RightParen, "Expected ')' after aggregate argument")
          .span.end;
  return {.function = function,
          .argument = std::move(argument),
          .span = {begin, end},
          .resolved_type = std::nullopt,
          .output_name = {}};
}

FromClause Parser::parse_from() {
  TableRef first = parse_table();
  if (check(TokenType::Join)) {
    JoinFrom from{.first = std::move(first), .joins = {}};
    while (match(TokenType::Join)) {
      Position begin = prev().span.begin;
      TableRef right = parse_table();
      consume(TokenType::On, "Expected ON after joined table");
      Condition on = parse_or();
      Span span{begin, on.span.end};
      from.joins.push_back(
          {.right = std::move(right), .on = std::move(on), .span = span});
    }
    return from;
  }
  CommaFrom from;
  from.tables.push_back(std::move(first));
  while (match(TokenType::Comma))
    from.tables.push_back(parse_table());
  return from;
}

TableRef Parser::parse_table() {
  Position begin = peek().span.begin;
  if (match(TokenType::LeftParen)) {
    Query query = parse_query();
    consume(TokenType::RightParen, "Expected ')' after subquery");
    consume(TokenType::As, "Expected AS and alias after subquery");
    NameWithSpan alias = parse_name();
    Span span{begin, alias.span.end};
    return {.value = SubqueryTable{.query = std::make_unique<Query>(
                                       std::move(query)),
                                   .alias = std::move(alias)},
            .span = span,
            .output_schema = std::nullopt};
  }
  NamedTable table{.name = parse_name(), .alias = std::nullopt};
  if (match(TokenType::As))
    table.alias = parse_name();
  Span span{begin, prev().span.end};
  return {
      .value = std::move(table), .span = span, .output_schema = std::nullopt};
}

GroupByClause Parser::parse_group_by() {
  Position begin = consume(TokenType::Group, "Expected GROUP").span.begin;
  consume(TokenType::By, "Expected BY after GROUP");
  GroupByClause group;
  group.columns.push_back(parse_column());
  while (match(TokenType::Comma))
    group.columns.push_back(parse_column());
  if (match(TokenType::Having))
    group.having = parse_or();
  group.span = {begin, prev().span.end};
  return group;
}

SortKey Parser::parse_sort_key() {
  ColumnRef column = parse_column();
  Position begin = column.span.begin;
  SortDirection direction = SortDirection::ASC;
  if (match(TokenType::Desc))
    direction = SortDirection::DESC;
  else
    match(TokenType::Asc);
  return {.column = std::move(column),
          .direction = direction,
          .span = {begin, prev().span.end},
          .output_position = std::nullopt};
}

ColumnRef Parser::parse_column() {
  NameWithSpan first = parse_name();
  if (match(TokenType::Dot)) {
    NameWithSpan name = parse_name();
    Span span{first.span.begin, name.span.end};
    return {.qualifier = std::move(first),
            .name = std::move(name),
            .span = span,
            .resolved_type = std::nullopt,
            .resolved_position = std::nullopt};
  }
  Span span = first.span;
  return {.qualifier = std::nullopt,
          .name = std::move(first),
          .span = span,
          .resolved_type = std::nullopt,
          .resolved_position = std::nullopt};
}

Condition Parser::parse_or() {
  Condition left = parse_and();
  while (match(TokenType::Or)) {
    Span operator_span = prev().span;
    Condition right = parse_and();
    Span span{left.span.begin, right.span.end};
    left = {.value =
                LogicalCondition{
                    .op = LogicalOperator::OR,
                    .left = std::make_unique<Condition>(std::move(left)),
                    .right = std::make_unique<Condition>(std::move(right)),
                    .operator_span = operator_span},
            .span = span};
  }
  return left;
}

Condition Parser::parse_and() {
  Condition left = parse_not();
  while (match(TokenType::And)) {
    Span operator_span = prev().span;
    Condition right = parse_not();
    Span span{left.span.begin, right.span.end};
    left = {.value =
                LogicalCondition{
                    .op = LogicalOperator::AND,
                    .left = std::make_unique<Condition>(std::move(left)),
                    .right = std::make_unique<Condition>(std::move(right)),
                    .operator_span = operator_span},
            .span = span};
  }
  return left;
}

Condition Parser::parse_not() {
  if (match(TokenType::Not)) {
    Span operator_span = prev().span;
    Condition child = parse_not();
    Span span{operator_span.begin, child.span.end};
    return {.value = NotCondition{.child = std::make_unique<Condition>(
                                      std::move(child)),
                                  .operator_span = operator_span},
            .span = span};
  }
  if (match(TokenType::LeftParen)) {
    Position begin = prev().span.begin;
    Condition condition = parse_or();
    Position end =
        consume(TokenType::RightParen, "Expected ')' after condition").span.end;
    condition.span = {begin, end};
    return condition;
  }
  return parse_comparison();
}

Condition Parser::parse_comparison() {
  Position begin = peek().span.begin;
  Operand left = parse_operand();
  Span operator_span = peek().span;
  ComparisonOperator op;
  if (match(TokenType::Equal))
    op = ComparisonOperator::EQUAL;
  else if (match(TokenType::NotEqual))
    op = ComparisonOperator::NOT_EQUAL;
  else if (match(TokenType::Less))
    op = ComparisonOperator::LESS_THAN;
  else if (match(TokenType::LessEqual))
    op = ComparisonOperator::LESS_THAN_OR_EQUAL;
  else if (match(TokenType::Greater))
    op = ComparisonOperator::GREATER_THAN;
  else if (match(TokenType::GreaterEqual))
    op = ComparisonOperator::GREATER_THAN_OR_EQUAL;
  else
    fail("Expected comparison operator");
  Operand right = parse_operand();
  Span span{begin, prev().span.end};
  return {.value = Comparison{.left = std::move(left),
                              .op = op,
                              .right = std::move(right),
                              .operator_span = operator_span},
          .span = span};
}

Operand Parser::parse_operand() {
  if (check(TokenType::Integer) || check(TokenType::String))
    return parse_literal();
  if (at_aggregate())
    return parse_aggregate();
  return parse_column();
}

} // namespace vql::query
