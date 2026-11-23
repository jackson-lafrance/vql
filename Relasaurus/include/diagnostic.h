#ifndef DIAGNOSTIC_H
#define DIAGNOSTIC_H

#include <stdexcept>
#include <string>
#include <utility>

enum class DiagnosticCategory {
  Name,
  Schema,
  Type,
};

inline const char *diagnostic_category_name(DiagnosticCategory category) {
  switch (category) {
  case DiagnosticCategory::Name:
    return "name";
  case DiagnosticCategory::Schema:
    return "schema";
  case DiagnosticCategory::Type:
    return "type";
  }

  return "runtime";
}

class DiagnosticError : public std::runtime_error {
public:
  DiagnosticError(DiagnosticCategory category, std::string message)
      : std::runtime_error(std::move(message)), category_(category) {}

  DiagnosticCategory category() const { return category_; }

private:
  DiagnosticCategory category_;
};

#endif // DIAGNOSTIC_H
