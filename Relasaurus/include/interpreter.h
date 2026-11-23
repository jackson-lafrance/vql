#ifndef INTERPRETER_H
#define INTERPRETER_H

#include "algebra.h"
#include "parser.h"
#include "relation.h"
#include <optional>
#include <string>
#include <unordered_map>

class Interpreter {
public:
  std::optional<Relation> execute(const Statement &statement);

  const OperationStats &stats() const;

private:
  Relation evaluate(const REX &expression);

  bool eval_cond(const Condition &condition, const Tuple &tuple,
                 const Schema &schema, const std::string &relation_name);

  Value eval_oppa(const Operand &operand, const Tuple &tuple,
                  const Schema &schema, const std::string &relation_name);

  std::unordered_map<std::string, Relation> relations_;
  OperationStats stats_;
};

#endif // INTERPRETER_H
