#include "lexer.h"
#include "diagnostic.h"

#include <charconv>
#include <utility>

namespace vql::query {

bool is_letter(char character) {
  return (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z') || character == '_';
}

bool is_digit(char character) { return character >= '0' && character <= '9'; }

TokenType keyword_type(std::string name) {
  for (char &character : name) {
    if (character >= 'a' && character <= 'z')
      character = static_cast<char>(character - 'a' + 'A');
  }
  const std::pair<const char *, TokenType> keywords[] = {
      {"CREATE", TokenType::Create},
      {"TABLE", TokenType::Table},
      {"INT", TokenType::Int},
      {"VARCHAR", TokenType::Varchar},
      {"INSERT", TokenType::Insert},
      {"INTO", TokenType::Into},
      {"VALUES", TokenType::Values},
      {"SELECT", TokenType::Select},
      {"DISTINCT", TokenType::Distinct},
      {"FROM", TokenType::From},
      {"WHERE", TokenType::Where},
      {"GROUP", TokenType::Group},
      {"BY", TokenType::By},
      {"HAVING", TokenType::Having},
      {"ORDER", TokenType::Order},
      {"ASC", TokenType::Asc},
      {"DESC", TokenType::Desc},
      {"LIMIT", TokenType::Limit},
      {"AS", TokenType::As},
      {"JOIN", TokenType::Join},
      {"ON", TokenType::On},
      {"UNION", TokenType::Union},
      {"ALL", TokenType::All},
      {"INTERSECT", TokenType::Intersect},
      {"EXCEPT", TokenType::Except},
      {"COUNT", TokenType::Count},
      {"SUM", TokenType::Sum},
      {"AVG", TokenType::Avg},
      {"MIN", TokenType::Min},
      {"MAX", TokenType::Max},
      {"AND", TokenType::And},
      {"OR", TokenType::Or},
      {"NOT", TokenType::Not}};
  for (const auto &[keyword, type] : keywords) {
    if (name == keyword)
      return type;
  }
  return TokenType::Identifier;
}

std::vector<Token> Lexer::tokenize(const std::string &source) {
  Lexer lexer(source);
  while (!lexer.at_end()) {
    lexer.token_start_ = lexer.current_;
    lexer.scan_token();
  }
  lexer.token_start_ = lexer.current_;
  lexer.emit(TokenType::EndOfInput);
  return std::move(lexer.tokens_);
}

Lexer::Lexer(const std::string &source) : source_(source) {}

bool Lexer::at_end(std::size_t lookahead) const {
  return lookahead >= source_.size() - current_.offset;
}

char Lexer::peek(std::size_t lookahead) const {
  return at_end(lookahead) ? '\0' : source_[current_.offset + lookahead];
}

char Lexer::advance() {
  char character = source_[current_.offset++];
  if (character == '\n') {
    ++current_.row;
    current_.col = 1;
  } else {
    ++current_.col;
  }
  return character;
}

void Lexer::emit(TokenType type, TokenValue value) {
  tokens_.push_back(
      {.type = type,
       .lexeme = source_.substr(token_start_.offset,
                                current_.offset - token_start_.offset),
       .value = std::move(value),
       .span = {token_start_, current_}});
}

void Lexer::scan_token() {
  char character = advance();
  switch (character) {
  case ' ':
  case '\t':
  case '\r':
  case '\n':
    return;
  case '(':
    emit(TokenType::LeftParen);
    return;
  case ')':
    emit(TokenType::RightParen);
    return;
  case ',':
    emit(TokenType::Comma);
    return;
  case '.':
    emit(TokenType::Dot);
    return;
  case ';':
    emit(TokenType::Semicolon);
    return;
  case '*':
    emit(TokenType::Star);
    return;
  case '=':
    emit(TokenType::Equal);
    return;
  case '<':
    if (peek() == '=') {
      advance();
      emit(TokenType::LessEqual);
    } else if (peek() == '>') {
      advance();
      emit(TokenType::NotEqual);
    } else {
      emit(TokenType::Less);
    }
    return;
  case '>':
    if (peek() == '=') {
      advance();
      emit(TokenType::GreaterEqual);
    } else {
      emit(TokenType::Greater);
    }
    return;
  case '\'':
    scan_string();
    return;
  case '-':
    if (peek() == '-') {
      while (!at_end() && peek() != '\n' && peek() != '\r')
        advance();
      return;
    }
    [[fallthrough]];
  case '+':
    if (!is_digit(peek()))
      fail("Expected digits after integer sign");
    scan_integer();
    return;
  default:
    if (is_digit(character)) {
      scan_integer();
    } else if (is_letter(character)) {
      scan_identifier();
    } else {
      fail("Unexpected character");
    }
  }
}

void Lexer::scan_identifier() {
  while (is_letter(peek()) || is_digit(peek()))
    advance();
  std::string name = source_.substr(token_start_.offset,
                                    current_.offset - token_start_.offset);
  TokenType type = keyword_type(name);
  if (type == TokenType::Identifier)
    emit(type, std::move(name));
  else
    emit(type);
}

void Lexer::scan_integer() {
  while (is_digit(peek()))
    advance();
  if (peek() == '.') {
    advance();
    fail("Only integer numbers are supported");
  }
  const char *begin = source_.data() + token_start_.offset;
  const char *end = source_.data() + current_.offset;
  if (*begin == '+')
    ++begin;
  std::int32_t value{};
  auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end)
    fail("Integer is outside the signed 32-bit range");
  emit(TokenType::Integer, value);
}

void Lexer::scan_string() {
  std::string value;
  while (!at_end()) {
    if (peek() == '\n' || peek() == '\r')
      fail("Newline in string literal");
    if (advance() == '\'') {
      if (peek() == '\'') {
        advance();
        value += '\'';
      } else {
        emit(TokenType::String, std::move(value));
        return;
      }
    } else {
      value += source_[current_.offset - 1];
    }
  }
  fail("Unterminated string literal");
}

void Lexer::fail(const std::string &message) const {
  throw DiagnosticError(ErrorCategory::LEXICAL, message,
                        {token_start_, current_});
}

} // namespace vql::query
