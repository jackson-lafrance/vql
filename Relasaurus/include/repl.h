#ifndef REPL_H
#define REPL_H

#include "parser.h"
#include "relation.h"

#include <iosfwd>
#include <optional>
#include <string>

enum class ParseStatus { Success, Incomplete, Error };

struct ParsedInput {
  ParseStatus status;
  Statement statement;
  std::optional<ParseError> incomplete_error;
};

ParsedInput parse_repl_source(const std::string &source);
void print_parse_error(const ParseError &error, const std::string &source);
void print_relation(const Relation &relation, std::ostream &output);
void print_tree(const REX &expression, std::ostream &output, int depth = 0);

#endif
