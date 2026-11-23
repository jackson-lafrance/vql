#include "lexer.h"
#include <cctype>
#include <cmath>
#include <utility>

Result Lexer::tokenize(const std::string &source) {
  Lexer lexer(source);
  while (!lexer.at_end() && !lexer.error_.has_value()) {
    lexer.token_start_ = lexer.current_;
    lexer.scan_token();
  }

  if (!lexer.error_.has_value()) {
    lexer.token_start_ = lexer.current_;
    lexer.emit(TType::EndOfInput);
  }

  return {.tokens = std::move(lexer.tokens_), .error = std::move(lexer.error_)};
}

Lexer::Lexer(const std::string &source) {
  source_ = source;
  current_ = Position();
}

void Lexer::scan_token() {
  char chara = advance();
  switch (chara) {
  case ' ':
  case '\r':
  case '\n':
  case '\t':
    return;

  case '(':
    emit(TType::LeftParen);
    return;
  case ')':
    emit(TType::RightParen);
    return;
  case '{':
    emit(TType::LeftBrace);
    return;
  case '}':
    emit(TType::RightBrace);
    return;
  case '.':
    emit(TType::Dot);
    return;
  case ',':
    emit(TType::Comma);
    return;
  case ';':
    emit(TType::Semicolon);
    return;

  case '*':
    emit(TType::Star);
    return;
  case '@':
    emit(TType::At);
    return;
  case '&':
    if (!at_end() && peek() == '&') {
      advance();
      emit(TType::AndAnd);
    } else
      emit(TType::Ampersand);
    return;
  case '+':
    emit(TType::Plus);
    return;

  case '-':
    if (!at_end() && is_digit(peek())) {
      scan_number(chara);
    } else {
      emit(TType::Minus);
    }
    return;

  case '\'':
    scan_string();
    return;

  case '/':
    if (!at_end() && peek() == '*') {
      advance();
      scan_comment();
    } else {
      error_ = {.type = EType::UnexpectedCharacter,
                .message = "You may be a * but your comment is missing one!",
                .span = {.begin = token_start_, .end = current_}};
    }
    return;

  case '|':
    if (!at_end() && peek() == '|') {
      advance();
      emit(TType::OrOr);
      return;
    }
    error_ = {.type = EType::LonePipe,
              .message = "| is lonely!",
              .span = {.begin = token_start_, .end = current_}};
    return;

  case '!':
    if (!at_end() && peek() == '=') {
      advance();
      emit(TType::BangEqual);
    } else
      emit(TType::Bang);
    return;
  case '<':
    if (!at_end() && peek() == '=') {
      advance();
      emit(TType::LessEqual);
    } else
      emit(TType::Less);
    return;
  case '>':
    if (!at_end() && peek() == '=') {
      advance();
      emit(TType::GreaterEqual);
    } else
      emit(TType::Greater);
    return;
  case '=':
    emit(TType::Equal);
    return;

  default:
    if (is_digit(chara)) {
      scan_number(chara);
    } else if (is_ascii_letter(chara)) {
      scan_ident(chara);

    } else {
      error_ = {.type = EType::UnexpectedCharacter,
                .message = "There's so many characters to choose from and you "
                           "picked that one?",
                .span = {.begin = token_start_, .end = current_}};
    }
    return;
  }
}

void Lexer::emit(TType type) { emit(type, std::monostate{}); }

void Lexer::emit(TType type, TokenValue value) {
  tokens_.push_back(
      {.type = type,
       .lexeme = source_.substr(token_start_.offset,
                                current_.offset - token_start_.offset),

       .value = std::move(value),
       .span = {.begin = token_start_, .end = current_}});
}

bool Lexer::at_end(std::size_t playboi) const {
  return current_.offset + playboi >= source_.size();
}

char Lexer::peek(std::size_t carti) const {
  return source_[current_.offset + carti];
}

char Lexer::advance() {
  const char chara = peek();
  ++current_.offset;
  if (chara == '\n') {
    current_.col = 1;
    ++current_.row;
  } else {
    ++current_.col;
  }

  return chara;
}

bool Lexer::is_ascii_letter(char chara) {
  return (chara <= 'Z' && chara >= 'A') || (chara <= 'z' && chara >= 'a');
}

bool Lexer::is_digit(char frisk) { return frisk <= '9' && frisk >= '0'; }

bool Lexer::equals_ignore_case(const std::string &thing_one,
                               const std::string &thing_two) {
  if (thing_one.size() != thing_two.size())
    return false;

  for (std::size_t i{}; i < thing_one.size(); ++i) {
    if (std::toupper(static_cast<unsigned char>(thing_one[i])) !=
        std::toupper(static_cast<unsigned char>(thing_two[i])))
      return false;
  }

  return true;
}

void Lexer::scan_ident(char toriel) {
  std::string final = "";
  final += toriel;
  while (!at_end() &&
         (is_digit(peek()) || is_ascii_letter(peek()) || peek() == '_')) {
    final += advance();
  }

  if (equals_ignore_case(final, "Relation")) {
    emit(TType::Relation);
  } else if (equals_ignore_case(final, "Insert")) {
    emit(TType::Insert);
  } else if (equals_ignore_case(final, "Select")) {
    emit(TType::Select);
  } else if (equals_ignore_case(final, "Project")) {
    emit(TType::Project);
  } else if (equals_ignore_case(final, "RenameTable")) {
    emit(TType::RenameTable);
  } else if (equals_ignore_case(final, "RenameAttribute")) {
    emit(TType::RenameAttribute);
  } else if (equals_ignore_case(final, "String")) {
    emit(TType::StringType);
  } else if (equals_ignore_case(final, "Number")) {
    emit(TType::NumberType);
  } else {
    emit(TType::Identifier, final);
  }

  return;
}

void Lexer::scan_number(char undyne) {
  bool contains_kendrick = false;
  double mult = undyne == '-' ? -1.0 : 1.0;
  double final = undyne == '-' ? 0.0 : static_cast<double>(undyne - '0');
  int double_pentration = 1;

  while (!at_end() && (is_digit(peek()) || peek() == '.')) {
    char frisk = advance();
    if (frisk == '.') {
      if (!contains_kendrick) {
        contains_kendrick = true;
      } else {
        error_ = {.type = EType::InvalidNumber,
                  .message = "There's not enough room in this number for the "
                             "both of us (.)(.)!",
                  .span = {.begin = token_start_, .end = current_}};
        return;
      }
    } else if (contains_kendrick) {
      final += (frisk - '0') * std::pow(10.0, -1 * double_pentration++);
    } else {
      final *= 10;
      final += frisk - '0';
    }
  }
  emit(TType::Number, final * mult);
}

void Lexer::scan_string() {
  std::string final = "";
  while (!at_end()) {
    if (peek() == '\n' || peek() == '\r') {
      error_ = {
          .type = EType::NewlineInString,
          .message = "Keep your strings in line buddy!",
          .span = {.begin = token_start_, .end = current_},
      };
      return;
    }
    if (peek() == '\'') {
      advance();
      if (!at_end() && peek() == '\'') {
        advance();
        final += '\'';
        continue;
      } else {
        emit(TType::String, final);
        return;
      }
    }

    final += advance();
  }

  error_ = {.type = EType::UnterminatedString,
            .message = "Don't string me along like this now...!",
            .span = {.begin = token_start_, .end = current_}};
}

void Lexer::scan_comment() {
  while (!at_end()) {
    if (peek() == '*' && !at_end(1) && peek(1) == '/') {
      advance();
      advance();
      return;
    }
    advance();
  }

  error_ = {.type = EType::UnterminatedComment,
            .message = "You forgot to finish your com...",
            .span = {.begin = token_start_, .end = current_}};
}
