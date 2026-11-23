#ifndef VQL_QUERY_DIAGNOSTIC_H
#define VQL_QUERY_DIAGNOSTIC_H

#include "source.h"

#include <stdexcept>
#include <string>

namespace vql::query {

enum class ErrorCategory {
  LEXICAL,
  SYNTAX,
  NAME,
  AMBIGUITY,
  TYPE,
  AGGREGATE,
  SCHEMA
};

std::string error_category_name(ErrorCategory category);

class DiagnosticError : public std::runtime_error {
public:
  DiagnosticError(ErrorCategory category, const std::string &message,
                  Span span);

  ErrorCategory category() const;
  Span span() const;

private:
  ErrorCategory category_;
  Span span_;
};

} // namespace vql::query

#endif // VQL_QUERY_DIAGNOSTIC_H
