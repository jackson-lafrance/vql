#ifndef VQL_QUERY_LEXER_H
#define VQL_QUERY_LEXER_H

#include "source.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace vql::query {

enum class TokenType {
  Identifier,
  Integer,
  String,
  Create,
  Table,
  Int,
  Varchar,
  Insert,
  Into,
  Values,
  Select,
  Distinct,
  From,
  Where,
  Group,
  By,
  Having,
  Order,
  Asc,
  Desc,
  Limit,
  As,
  Join,
  On,
  Union,
  All,
  Intersect,
  Except,
  Count,
  Sum,
  Avg,
  Min,
  Max,
  And,
  Or,
  Not,
  LeftParen,
  RightParen,
  Comma,
  Dot,
  Semicolon,
  Star,
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  EndOfInput
};

using TokenValue = std::variant<std::monostate, std::int32_t, std::string>;

struct Token {
  TokenType type;
  std::string lexeme;
  TokenValue value;
  Span span;
};

class Lexer {
public:
  static std::vector<Token> tokenize(const std::string &source);

private:
  explicit Lexer(const std::string &source);

  bool at_end(std::size_t lookahead = 0) const;
  char peek(std::size_t lookahead = 0) const;
  char advance();
  void emit(TokenType type, TokenValue value = std::monostate{});
  void scan_token();
  void scan_identifier();
  void scan_integer();
  void scan_string();
  void fail(const std::string &message) const;

  const std::string &source_;
  Position token_start_;
  Position current_;
  std::vector<Token> tokens_;
};

} // namespace vql::query

#endif // VQL_QUERY_LEXER_H
