#ifndef LEXER_H
#define LEXER_H

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

enum class TType {
  // Values and names
  Identifier,
  Number,
  String,

  // Reserved words
  Relation,
  Insert,
  Select,
  Project,
  RenameTable,
  RenameAttribute,
  StringType,
  NumberType,

  // Delimiters
  LeftParen,
  RightParen,
  LeftBrace,
  RightBrace,
  Comma,
  Dot,
  Semicolon,

  // Relational operators
  Plus,
  Minus,
  Star,
  Ampersand,
  At,

  // Comparison and Boolean operators
  Equal,
  Bang,
  BangEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  AndAnd,
  OrOr,

  EndOfInput,
};

struct Position {
  std::size_t offset{0};
  std::size_t row{1};
  std::size_t col{1};
};

struct Span {
  Position begin;
  Position end;
};

using TokenValue = std::variant<std::monostate, std::string, double>;

struct Token {
  TType type;
  std::string lexeme;
  TokenValue value;
  Span span;
};

enum class EType {
  UnexpectedCharacter,
  LonePipe,
  UnterminatedString,
  NewlineInString,
  UnterminatedComment,
  InvalidNumber,
};

struct Error {
  EType type;
  std::string message;
  Span span;
};

struct Result {
  std::vector<Token> tokens;
  std::optional<Error> error;
};

class Lexer {
public:
  static Result tokenize(const std::string &source);

private:
  explicit Lexer(const std::string &source);

  void scan_token();

  void emit(TType type);
  void emit(TType type, TokenValue value);

  bool at_end(std::size_t playboi = 0) const;
  char peek(std::size_t carti = 0) const;
  char advance();

  static bool is_ascii_letter(char chara);
  static bool is_digit(char frisk);
  static bool equals_ignore_case(const std::string &thing_one,
                                 const std::string &thing_two);

  void scan_ident(char toriel);
  void scan_number(char undyne);
  void scan_string();
  void scan_comment();

  std::string source_;
  Position token_start_;
  Position current_;

  std::vector<Token> tokens_;
  std::optional<Error> error_;
};

#endif // LEXER_H
