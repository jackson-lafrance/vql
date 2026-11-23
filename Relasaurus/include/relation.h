#ifndef RELATION_H
#define RELATION_H

#include "schema.h"
#include <cstddef>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

using Value = std::variant<double, std::string>;
using Tuple = std::vector<Value>;

class TupleCollection {
public:
  using const_iterator = std::vector<Tuple>::const_iterator;

  bool contains(const Tuple &tuple) const;
  std::size_t size() const;
  const_iterator begin() const;
  const_iterator end() const;

private:
  friend class Relation;

  static bool values_equal(const Value &left, const Value &right);
  static bool tuples_equal(const Tuple &left, const Tuple &right);
  static std::size_t hash_value(const Value &value);
  static std::size_t hash_tuple(const Tuple &tuple);
  void insert_unique(const Tuple &tuple);

  std::vector<Tuple> tuples_;
  std::unordered_map<std::size_t, std::vector<std::size_t>> buckets_;
};

class Relation {
public:
  Relation(std::string n, Schema schem);

  void insert_row(const Tuple &tuple);
  bool contains(const Tuple &tuple) const;

  const TupleCollection &tuples() const;
  const Schema &schema() const;
  const std::string &name() const;

private:
  std::string name_;
  Schema schema_;
  TupleCollection tuples_;

  void validate_row(const Tuple &tuple);
};

#endif // RELATION_H
