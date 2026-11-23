#include "diagnostic.h"

namespace vql::query {

std::string error_category_name(ErrorCategory category) {
  switch (category) {
  case ErrorCategory::LEXICAL:
    return "lexical";
  case ErrorCategory::SYNTAX:
    return "syntax";
  case ErrorCategory::NAME:
    return "name";
  case ErrorCategory::AMBIGUITY:
    return "ambiguity";
  case ErrorCategory::TYPE:
    return "type";
  case ErrorCategory::AGGREGATE:
    return "aggregate";
  case ErrorCategory::SCHEMA:
    return "schema";
  }
  throw std::invalid_argument("Unknown error category!");
}

DiagnosticError::DiagnosticError(ErrorCategory category,
                                 const std::string &message, Span span)
    : std::runtime_error("error: " + error_category_name(category) + ": " +
                         message + " at line " +
                         std::to_string(span.begin.row) + ", column " +
                         std::to_string(span.begin.col)),
      category_(category), span_(span) {}

ErrorCategory DiagnosticError::category() const { return category_; }

Span DiagnosticError::span() const { return span_; }

} // namespace vql::query
