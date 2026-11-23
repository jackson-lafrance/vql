#include "relation.h"
#include "diagnostic.h"
#include "schema.h"
#include <functional>
#include <stdexcept>
#include <utility>

bool TupleCollection::values_equal(const Value &left, const Value &right) {
  if (left.index() != right.index())
    return false;

  if (const auto *left_number = std::get_if<double>(&left))
    return *left_number == std::get<double>(right);

  return std::get<std::string>(left) == std::get<std::string>(right);
}

bool TupleCollection::tuples_equal(const Tuple &left, const Tuple &right) {
  if (left.size() != right.size())
    return false;

  for (std::size_t i = 0; i < left.size(); ++i)
    if (!values_equal(left[i], right[i]))
      return false;

  return true;
}

std::size_t TupleCollection::hash_value(const Value &value) {
  constexpr std::size_t hash_magic = 0x9e3779b9;
  const std::size_t item_hash =
      std::holds_alternative<double>(value)
          ? std::hash<double>{}(std::get<double>(value))
          : std::hash<std::string>{}(std::get<std::string>(value));
  const std::size_t type_hash = value.index();

  return type_hash ^
         (item_hash + hash_magic + (type_hash << 6) + (type_hash >> 2));
}

std::size_t TupleCollection::hash_tuple(const Tuple &tuple) {
  constexpr std::size_t hash_magic = 0x9e3779b9;
  std::size_t hash = tuple.size();

  for (const Value &value : tuple) {
    const std::size_t item_hash = hash_value(value);
    hash ^= item_hash + hash_magic + (hash << 6) + (hash >> 2);
  }

  return hash;
}

bool TupleCollection::contains(const Tuple &tuple) const {
  const auto bucket = buckets_.find(hash_tuple(tuple));
  if (bucket == buckets_.end())
    return false;

  for (const std::size_t index : bucket->second)
    if (tuples_equal(tuples_[index], tuple))
      return true;

  return false;
}

void TupleCollection::insert_unique(const Tuple &tuple) {
  const std::size_t hash = hash_tuple(tuple);
  std::vector<std::size_t> &bucket = buckets_[hash];

  for (const std::size_t index : bucket)
    if (tuples_equal(tuples_[index], tuple))
      return;

  bucket.reserve(bucket.size() + 1);
  tuples_.push_back(tuple);
  bucket.push_back(tuples_.size() - 1);
}

std::size_t TupleCollection::size() const { return tuples_.size(); }

TupleCollection::const_iterator TupleCollection::begin() const {
  return tuples_.begin();
}

TupleCollection::const_iterator TupleCollection::end() const {
  return tuples_.end();
}

Relation::Relation(std::string n, Schema s)
    : name_(std::move(n)), schema_(std::move(s)) {}

void Relation::insert_row(const Tuple &tuple) {
  validate_row(tuple);
  tuples_.insert_unique(tuple);
}

bool Relation::contains(const Tuple &tuple) const {
  return tuples_.contains(tuple);
}

const TupleCollection &Relation::tuples() const { return tuples_; }

const std::string &Relation::name() const { return name_; }
const Schema &Relation::schema() const { return schema_; }

void Relation::validate_row(const Tuple &tuple) {
  if (tuple.size() != schema_.columns().size()) {
    throw DiagnosticError(DiagnosticCategory::Schema,
                          "TUPLE LENGTH != ATTRIBUTES LENGTH");
  }
  for (std::size_t i{}; i < tuple.size(); ++i) {
    bool matches = false;
    switch (schema_.columns()[i].type) {
    case STRING:
      matches = std::holds_alternative<std::string>(tuple[i]);
      break;
    case NUMBER:
      matches = std::holds_alternative<double>(tuple[i]);
      break;
    }
    if (!matches)
      throw DiagnosticError(DiagnosticCategory::Type,
                            "TUPLE TYPES DO NOT MATCH SCHEMA");
  }
}
