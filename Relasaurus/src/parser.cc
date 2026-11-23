#include "parser.h"
#include "lexer.h"
#include "schema.h"
#include <cctype>

std::variant<Statement, ParseError>
Parser::parse_tokens(std::vector<Token> &tokens) {
  Parser parser(tokens);

  try {
    Statement statement = parser.parse_statement();
    parser.consume(TType::EndOfInput,
                   "EXPECTED END OF INPUT AFTER STATEMENT!!!!");
    return std::move(statement);
  } catch (ParseError error) {
    return std::move(error);
  }
}

Parser::Parser(std::vector<Token> &tokens) : tokens_(tokens) {}

const Token &Parser::peek() const { return tokens_[current_]; }
const Token &Parser::prev() const { return tokens_[current_ - 1]; }
const Token &Parser::advance() {
  const Token &token = peek();

  if (!at_end())
    ++current_;

  return token;
}

bool Parser::at_end() const { return peek().type == TType::EndOfInput; }
bool Parser::check(TType type) const { return peek().type == type; }
bool Parser::match(TType type) {
  if (!check(type)) {
    return false;
  }

  advance();
  return true;
}

const Token &Parser::consume(TType type, std::string error) {
  if (check(type))
    return advance();

  fail(peek(), type, std::move(error));
}

Statement Parser::parse_statement() {
  if (check(TType::Relation)) {
    return parse_relation_definition();
  }

  if (check(TType::Insert)) {
    return parse_relation_insertion();
  }

  return parse_query_statement();
}

Statement Parser::parse_relation_definition() {
  const Token &start =
      consume(TType::Relation, "I swear that was a 'relation'...");
  const Token &name = consume(TType::Identifier, "");

  consume(TType::LeftParen, "Expected '(' to open relation schema!!");
  std::vector<ColumnWithSpan> columns = parse_schema();
  consume(TType::RightParen, "Expected ')' to close relation schema!!");

  consume(TType::LeftBrace, "Expected '{' to open tuples definition!!");
  std::vector<TupleWithSpan> tuples;
  while (!check(TType::RightBrace)) {
    if (at_end()) {
      fail(peek(), TType::RightBrace, "Expected '}' after relation tuples!!");
    }
    tuples.push_back(parse_tuple());
  }
  consume(TType::RightBrace, "Expected '}' to close tuples definition!!");

  const Token &semicolon =
      consume(TType::Semicolon, "Expected ';' after relation definition!!");

  return Statement{
      .guy =
          RelationDefinition{
              .name = {.name = name.lexeme, .span = name.span},
              .columns = std::move(columns),
              .tuples = std::move(tuples),
          },
      .span = {.begin = start.span.begin, .end = semicolon.span.end}};
};

Statement Parser::parse_relation_insertion() {
  const Token &start =
      consume(TType::Insert, "Hey what'd you do with my insert statement??");
  const Token &name =
      consume(TType::Identifier, "Expected relation name after insert!!");

  consume(TType::LeftBrace, "Expected '{' after relation name!!");
  std::vector<TupleWithSpan> tuples;
  while (!check(TType::RightBrace)) {
    if (at_end()) {
      fail(peek(), TType::RightBrace, "Expected '}' after relation tuples!!");
    }
    tuples.push_back(parse_tuple());
  }
  consume(TType::RightBrace, "Expected '}' to close tuples definition!!");

  const Token &semicolon =
      consume(TType::Semicolon, "Expected ';' after relation definition!!");

  return Statement{
      .guy =
          RelationInsertion{
              .name = {.name = name.lexeme, .span = name.span},
              .tuples = std::move(tuples),
          },
      .span = {.begin = start.span.begin, .end = semicolon.span.end}};
};

Statement Parser::parse_query_statement() {
  const Position start = peek().span.begin;

  REX query = parse_rex();

  const Token &semicolon =
      consume(TType::Semicolon, "Expected ';' after query!!");

  return Statement{.guy = std::move(query),
                   .span = {.begin = start, .end = semicolon.span.end}};
}

REX Parser::parse_rex() {
  REX left = parse_primary();

  while (true) {
    if (match(TType::At)) {
      consume(TType::LeftBrace, "Expected '{' after '@'!!");
      Condition condition = parse_or();
      consume(TType::RightBrace, "Expected '}' after join condition!!");

      REX right = parse_primary();

      const Position begin = left.span.begin;
      const Position end = right.span.end;

      left = REX{
          .node =
              JoinExpression{.condition = std::move(condition),
                             .left = std::make_unique<REX>(std::move(left)),
                             .right = std::make_unique<REX>(std::move(right))},
          .span = {.begin = begin, .end = end}};

      continue;
    };

    BinaryOperator operation;

    if (match(TType::Plus)) {
      operation = BinaryOperator::UNION;
    } else if (match(TType::Ampersand)) {
      operation = BinaryOperator::INTERSECT;
    } else if (match(TType::Minus)) {
      operation = BinaryOperator::MINUS;
    } else if (match(TType::Star)) {
      operation = BinaryOperator::TIMES;
    } else {
      break;
    }

    REX right = parse_primary();

    const Position begin = left.span.begin;
    const Position end = right.span.end;

    left = REX{
        .node =
            BinaryExpression{.operation = operation,
                             .left = std::make_unique<REX>(std::move(left)),
                             .right = std::make_unique<REX>(std::move(right))},
        .span = {.begin = begin, .end = end}};
  }

  return left;
}

REX Parser::parse_primary() {
  if (check(TType::Identifier)) {
    const Token &name = advance();

    return REX{.node = NameWithSpan{.name = name.lexeme, .span = name.span},
               .span = name.span};
  }

  if (check(TType::LeftParen)) {
    const Position begin = advance().span.begin;

    REX expression = parse_rex();

    const Position end =
        consume(TType::RightParen, "Expected ')' after expression!!").span.end;

    expression.span = {.begin = begin, .end = end};

    return expression;
  }

  if (check(TType::Select)) {
    return parse_select();
  }

  if (check(TType::Project)) {
    return parse_project();
  }

  if (check(TType::RenameTable)) {
    return parse_rename_table();
  }

  if (check(TType::RenameAttribute)) {
    return parse_rename_attribute();
  }

  fail(peek(), "Expected a relation or relational expression!!");
};

REX Parser::parse_select() {
  const Position begin =
      consume(TType::Select, "What did you do with my SELECT??").span.begin;

  consume(TType::LeftBrace, "Expected a '{' after SELECT!!");
  Condition condition = parse_or();
  consume(TType::RightBrace, "Expected a '}' after condition!!");

  consume(TType::LeftParen, "Expected a '(' before expression!!");
  REX inner = parse_rex();
  const Position end =
      consume(TType::RightParen, "Expected ')' after expression!!").span.end;

  return REX{.node = SelectExpression{.condition = std::move(condition),
                                      .input = std::make_unique<REX>(
                                          std::move(inner))},
             .span = {.begin = begin, .end = end}};
}

REX Parser::parse_project() {
  const Position begin =
      consume(TType::Project, "What did you do with my PROJECT??").span.begin;

  consume(TType::LeftBrace, "Expected a '{' after PROJECT!!");
  std::vector<AttributeReference> popeye;
  popeye.push_back(parse_attribute_reference());
  while (match(TType::Comma))
    popeye.push_back(parse_attribute_reference());
  consume(TType::RightBrace, "Expected a '}' after attribute list!!");

  consume(TType::LeftParen, "Expected a '(' before expression!!");
  REX inner = parse_rex();
  const Position end =
      consume(TType::RightParen, "Expected ')' after expression!!").span.end;

  return REX{.node = ProjectExpression{.attributes = std::move(popeye),
                                       .input = std::make_unique<REX>(
                                           std::move(inner))},
             .span = {.begin = begin, .end = end}};
};

REX Parser::parse_rename_table() {
  const Position begin =
      consume(TType::RenameTable, "What did you do with my RENAMETABLE??")
          .span.begin;

  consume(TType::LeftBrace, "Expected a '{' after RENAMETABLE!!");
  const Token &new_name =
      consume(TType::Identifier, "Expected a new name for the relation!!");
  consume(TType::RightBrace, "Expected a '}' after new relation name!!");

  consume(TType::LeftParen, "Expected a '(' before expression!!");
  REX inner = parse_rex();
  const Position end =
      consume(TType::RightParen, "Expected ')' after expression!!").span.end;

  return REX{.node =
                 RenameTableExpression{
                     .new_name = NameWithSpan{.name = new_name.lexeme,
                                              .span = new_name.span},
                     .input = std::make_unique<REX>(std::move(inner))},
             .span = {.begin = begin, .end = end}};
}

REX Parser::parse_rename_attribute() {
  const Position begin = consume(TType::RenameAttribute,
                                 "What did you do with my RENAMEATTRIBUTE??")
                             .span.begin;

  consume(TType::LeftBrace, "Expected a '{' after RENAMETABLE!!");
  const Token &new_name =
      consume(TType::Identifier, "Expected a new name for an attribute!!");
  consume(TType::Comma, "Expected a comma after the new name!!");
  AttributeReference old_name = parse_attribute_reference();
  consume(TType::RightBrace, "Expected a '}' after attribute names!!");

  consume(TType::LeftParen, "Expected a '(' before expression!!");
  REX inner = parse_rex();
  const Position end =
      consume(TType::RightParen, "Expected ')' after expression!!").span.end;

  return REX{.node =
                 RenameAttributeExpression{
                     .new_name = NameWithSpan{.name = new_name.lexeme,
                                              .span = new_name.span},
                     .old_name = std::move(old_name),
                     .input = std::make_unique<REX>(std::move(inner))},
             .span = {.begin = begin, .end = end}};
}

Condition Parser::parse_or() {
  Condition left = parse_and();

  while (match(TType::OrOr)) {
    Condition right = parse_and();

    const Span span{.begin = left.span.begin, .end = right.span.end};

    left = Condition{
        .node =
            LogicalCondition{
                .left = std::make_unique<Condition>(std::move(left)),
                .right = std::make_unique<Condition>(std::move(right)),
                .king_von = LogicalOperator::Or},
        .span = span};
  }

  return left;
}

Condition Parser::parse_and() {
  Condition left = parse_not();

  while (match(TType::AndAnd)) {
    Condition right = parse_not();

    const Span span{.begin = left.span.begin, .end = right.span.end};

    left = Condition{
        .node =
            LogicalCondition{
                .left = std::make_unique<Condition>(std::move(left)),
                .right = std::make_unique<Condition>(std::move(right)),
                .king_von = LogicalOperator::And},
        .span = span};
  }

  return left;
}

Condition Parser::parse_not() {
  if (check(TType::Bang)) {
    const Position start = advance().span.begin;
    Condition condition = parse_not();

    return Condition{.node =
                         NotCondition{.operand = std::make_unique<Condition>(
                                          std::move(condition))},
                     .span = Span{.begin = start, .end = condition.span.end}};
  } else if (check(TType::LeftParen)) {
    const Position start = advance().span.begin;
    Condition condition = parse_or();
    const Position end =
        consume(TType::RightParen, "Expected ')' after condition!!").span.end;
    condition.span = {.begin = start, .end = end};

    return condition;
  }
  return parse_comparison();
};

Condition Parser::parse_comparison() {
  Operand left = parse_operand();

  ComparisonOperator operation;

  if (match(TType::Equal)) {
    operation = ComparisonOperator::Equal;
  } else if (match(TType::BangEqual)) {
    operation = ComparisonOperator::NotEqual;
  } else if (match(TType::Less)) {
    operation = ComparisonOperator::Less;
  } else if (match(TType::LessEqual)) {
    operation = ComparisonOperator::LessEqual;
  } else if (match(TType::Greater)) {
    operation = ComparisonOperator::Greater;
  } else if (match(TType::GreaterEqual)) {
    operation = ComparisonOperator::GreaterEqual;
  } else {
    fail(peek(), "Expected a comparison operator!!");
  }

  Operand right = parse_operand();

  return Condition{.node = ComparisonCondition{.left = std::move(left),
                                               .right = std::move(right),
                                               .lil_durk = operation},
                   .span =
                       Span{.begin = left.span.begin, .end = right.span.end}};
}

Operand Parser::parse_operand() {
  if (match(TType::Number)) {
    return Operand{.value = std::get<double>(prev().value),
                   .span = prev().span};
  }

  if (match(TType::String)) {
    return Operand{.value = std::get<std::string>(prev().value),
                   .span = prev().span};
  }

  if (check(TType::Identifier)) {
    AttributeReference attr = parse_attribute_reference();
    const Span span = attr.span;

    return Operand{
        .value = std::move(attr),
        .span = span,
    };
  }

  fail(peek(),
       "Expected a number, string, or attribute reference as an operand!!");
};

AttributeReference Parser::parse_attribute_reference() {
  const Token &first = consume(TType::Identifier, "Expected an attribute!!");

  if (match(TType::Dot)) {
    const Token &attr_name =
        consume(TType::Identifier, "Expected an attribute after '.'!!");
    return {.relation_name = NameWithSpan{first.lexeme, first.span},
            .attribute_name = NameWithSpan{attr_name.lexeme, attr_name.span},
            .span = Span{.begin = first.span.begin, .end = attr_name.span.end}};
  }

  return {.relation_name = std::nullopt,
          .attribute_name = NameWithSpan{first.lexeme, first.span},
          .span = first.span};
}

std::vector<ColumnWithSpan> Parser::parse_schema() {
  std::vector<ColumnWithSpan> columns;
  columns.push_back(parse_column());

  while (match(TType::Comma)) {
    if (check(TType::RightParen))
      break;

    columns.push_back(parse_column());
  }

  return columns;
}

ColumnWithSpan Parser::parse_column() {
  const Token &name =
      consume(TType::Identifier, "Expected an attribute name!!");

  consume(TType::Equal, "Expected '=' after attribute name!!");

  Type type;
  if (match(TType::StringType)) {
    type = STRING;
  } else if (match(TType::NumberType)) {
    type = NUMBER;
  } else {
    fail(peek(), "Expected 'string' or 'number' after '='!!");
  }

  const Token &type_token = prev();

  return ColumnWithSpan{
      .column = {.name = name.lexeme, .type = type},
      .span = {.begin = name.span.begin, .end = type_token.span.end},
  };
}

TupleWithSpan Parser::parse_tuple() {
  Position position = peek().span.begin;
  Tuple values;
  values.push_back(parse_value());

  while (match(TType::Comma))
    values.push_back(parse_value());

  const Token &semicolon =
      consume(TType::Semicolon, "Expected ';' after tuple!!");

  return TupleWithSpan{.tuple = values,
                       .span = {.begin = position, .end = semicolon.span.end}};
}

Value Parser::parse_value() {
  if (match(TType::Number))
    return std::get<double>(prev().value);

  if (match(TType::String))
    return std::get<std::string>(prev().value);

  if (match(TType::Identifier))
    return prev().lexeme;

  fail(peek(), "Expected a tuple value.");
}

[[noreturn]]
void Parser::fail(const Token &token, std::string message) {
  throw ParseError{
      .message = std::move(message),
      .span = token.span,
      .actual = token.type,
      .expected = std::nullopt,
  };
}

[[noreturn]]
void Parser::fail(const Token &token, TType expected, std::string message) {
  throw ParseError{
      .message = std::move(message),
      .span = token.span,
      .actual = token.type,
      .expected = expected,
  };
}
