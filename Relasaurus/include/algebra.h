#ifndef ALGEBRA_H
#define ALGEBRA_H

#include "relation.h"
#include "schema.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using Predicate =
    std::function<bool(const Tuple &, const Schema &, const std::string &)>;
using ConditionValidator =
    std::function<void(const Schema &, const std::string &)>;

struct OperationStats {
  std::uint64_t join_comparisons{0};
  std::uint64_t selection_examinations{0};
};

class Algebra {
public:
  static Relation selection(const Relation &relation, Predicate predicate,
                            OperationStats *stats = nullptr);
  static Relation projection(const Relation &relation,
                             const std::vector<std::string> &attributes);

  static Relation rename(const Relation &relation, std::string new_name);
  static Relation rename(const Relation &relation, std::string old_name,
                         std::string new_name);

  static Relation times(const Relation &rel_1, const Relation &rel_2,
                        std::string modifier = "TIMES");
  static Relation join(const Relation &rel_1, const Relation &rel_2,
                       Predicate predicate, OperationStats *stats = nullptr,
                       ConditionValidator validator = nullptr);

  static Relation onion(const Relation &rel_1, const Relation &rel_2);
  static Relation intersect(const Relation &rel_1, const Relation &rel_2);
  static Relation minus(const Relation &rel_1, const Relation &rel_2);

private:
  static bool compare_schemas(const Relation &rel_1, const Relation &rel_2);
};

#endif // ALGEBRA_H
