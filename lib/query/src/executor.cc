#include "executor.h"

#include "operators/distinct_operator.h"
#include "operators/except_operator.h"
#include "operators/filter_operator.h"
#include "operators/groupby_operator.h"
#include "operators/hash_join_operator.h"
#include "operators/intersect_operator.h"
#include "operators/join_operator.h"
#include "operators/limit_operator.h"
#include "operators/projection_operator.h"
#include "operators/sequential_operator.h"
#include "operators/sort_merge_join_operator.h"
#include "operators/sort_operator.h"
#include "operators/unified_operator.h"
#include "operators/union_operator.h"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

namespace vql::query::executor {

using Node = std::unique_ptr<algebra::Operator>;

static std::string text(const Json::Value &value, const std::string &field) {
  if (!value.isString() || value.asString().empty())
    throw std::invalid_argument(field + " must be a nonempty string");
  return value.asString();
}

void array(const Json::Value &value, const std::string &field) {
  if (!value.isArray())
    throw std::invalid_argument(field + " must be an array");
}

std::vector<std::string> names(const Json::Value &value,
                               const std::string &field) {
  array(value, field);
  std::vector<std::string> result;
  for (const auto &item : value)
    result.push_back(text(item, field + " entry"));
  return result;
}

std::string qualify(const std::string &qualifier, const std::string &name) {
  const auto dot = name.rfind('.');
  return qualifier + "." +
         (dot == std::string::npos ? name : name.substr(dot + 1));
}

algebra::Schema renamed(const algebra::Schema &schema,
                        const std::string &qualifier) {
  std::vector<algebra::Column> columns;
  for (std::size_t i = 0; i < schema.size(); ++i)
    columns.push_back({qualify(qualifier, schema[i].name), schema[i].type});
  return algebra::Schema(std::move(columns));
}

class Relabel final : public algebra::Operator {
public:
  Relabel(Node input, algebra::Schema schema) : input_(std::move(input)) {
    schema_ = std::move(schema);
  }
  const algebra::Schema &schema() const override { return schema_; }
  void open() override { input_->open(); }
  void close() override { input_->close(); }
  std::optional<algebra::Tuple> next() override {
    auto row = input_->next();
    if (!row)
      return std::nullopt;
    ++counter_.tuples_out;
    return row->with_schema(schema_);
  }
  Counter counters() const override { return counter_; }

private:
  Node input_;
};

class OperatorWithContext final : public algebra::Operator {
public:
  OperatorWithContext(Node input, std::string path)
      : input_(std::move(input)), path_(std::move(path)) {}
  const algebra::Schema &schema() const override { return input_->schema(); }
  Counter counters() const override { return input_->counters(); }

  void open() override {
    needs_close_ = true;
    try {
      input_->open();
    } catch (const std::exception &error) {
      throw std::runtime_error(path_ + ": " + error.what());
    }
  }

  std::optional<algebra::Tuple> next() override {
    try {
      return input_->next();
    } catch (const std::exception &error) {
      throw std::runtime_error(path_ + ": " + error.what());
    }
  }

  void close() override {
    if (!needs_close_)
      return;
    try {
      input_->close();
      needs_close_ = false;
    } catch (const std::exception &error) {
      throw std::runtime_error(path_ + ": " + error.what());
    }
  }

  ~OperatorWithContext() override {
    try {
      close();
    } catch (...) {
    }
  }

private:
  Node input_;
  std::string path_;
  bool needs_close_{false};
};

using Predicate = std::function<bool(const algebra::Tuple &)>;
using Key = std::pair<std::size_t, std::size_t>;

void require_object(const Json::Value &value, const char *field) {
  if (!value.isObject())
    throw std::invalid_argument(std::string(field) + " must be an object");
}

struct BoundOperand {
  std::variant<std::size_t, std::int32_t, std::string> source;
  algebra::DataType type;

  algebra::Value read(const algebra::Tuple &row) const {
    if (const auto *i = std::get_if<std::size_t>(&source))
      return row[*i];
    if (const auto *number = std::get_if<std::int32_t>(&source))
      return *number;
    return std::string_view(std::get<std::string>(source));
  }
};

BoundOperand bind_operand(const Json::Value &operand,
                          const algebra::Schema &schema) {
  require_object(operand, "condition operand");
  const auto &kind = operand["kind"];
  if (kind == "column" || kind == "aggregate") {
    const auto i = schema.index_of(text(operand["name"], "operand name"));
    return {i, schema[i].type.data_type()};
  }
  const auto &value = operand["value"];
  if (kind == "integer") {
    if ((value.type() != Json::intValue && value.type() != Json::uintValue) ||
        !value.isInt())
      throw std::invalid_argument("condition literal must be a 32-bit integer");
    return {static_cast<std::int32_t>(value.asInt()), algebra::DataType::INT};
  }
  if (kind == "varchar") {
    if (!value.isString())
      throw std::invalid_argument("VARCHAR literal must be a string");
    return {value.asString(), algebra::DataType::VARCHAR};
  }
  throw std::invalid_argument("unknown condition operand kind");
}

enum class Comparison {
  EQUAL,
  NOT_EQUAL,
  LESS,
  LESS_EQUAL,
  GREATER,
  GREATER_EQUAL
};

Comparison comparison(const Json::Value &op) {
  if (op == "=")
    return Comparison::EQUAL;
  if (op == "<>")
    return Comparison::NOT_EQUAL;
  if (op == "<")
    return Comparison::LESS;
  if (op == "<=")
    return Comparison::LESS_EQUAL;
  if (op == ">")
    return Comparison::GREATER;
  if (op == ">=")
    return Comparison::GREATER_EQUAL;
  throw std::invalid_argument("unknown comparison operator");
}

struct ComparisonPredicate {
  BoundOperand left;
  BoundOperand right;
  Comparison op;

  bool operator()(const algebra::Tuple &row) const {
    int order = algebra::compare_values(left.read(row), right.read(row));
    switch (op) {
    case Comparison::EQUAL:
      return order == 0;
    case Comparison::NOT_EQUAL:
      return order != 0;
    case Comparison::LESS:
      return order < 0;
    case Comparison::LESS_EQUAL:
      return order <= 0;
    case Comparison::GREATER:
      return order > 0;
    case Comparison::GREATER_EQUAL:
      return order >= 0;
    }
    throw std::logic_error("invalid comparison operator");
  }
};

struct NotPredicate {
  Predicate child;

  bool operator()(const algebra::Tuple &row) const { return !child(row); }
};

struct LogicalPredicate {
  Predicate left;
  Predicate right;
  bool is_and;

  bool operator()(const algebra::Tuple &row) const {
    if (is_and)
      return left(row) && right(row);
    return left(row) || right(row);
  }
};

Predicate bind(const Json::Value &condition, const algebra::Schema &schema) {
  require_object(condition, "condition");
  const auto &kind = condition["kind"];
  if (kind == "not")
    return NotPredicate{bind(condition["child"], schema)};
  if (kind == "logical") {
    const auto &op = condition["operator"];
    if (op != "and" && op != "or")
      throw std::invalid_argument("logical operator must be 'and' or 'or'");
    return LogicalPredicate{bind(condition["left"], schema),
                            bind(condition["right"], schema), op == "and"};
  }
  if (kind != "comparison")
    throw std::invalid_argument("unknown condition kind");
  auto left = bind_operand(condition["left"], schema);
  auto right = bind_operand(condition["right"], schema);
  if (left.type != right.type)
    throw std::invalid_argument("condition cannot compare INT and VARCHAR");
  return ComparisonPredicate{std::move(left), std::move(right),
                             comparison(condition["operator"])};
}

void equi_keys(const Json::Value &condition, const algebra::Schema &schema,
               std::size_t left_size, std::vector<Key> &keys) {
  require_object(condition, "condition");
  const auto &kind = condition["kind"];
  const auto &op = condition["operator"];
  if (kind == "logical" && op == "and") {
    equi_keys(condition["left"], schema, left_size, keys);
    equi_keys(condition["right"], schema, left_size, keys);
    return;
  }
  const auto &left = condition["left"];
  const auto &right = condition["right"];
  if (kind != "comparison" || op != "=" || !left.isObject() ||
      !right.isObject() || left["kind"] != "column" ||
      right["kind"] != "column")
    throw std::invalid_argument(
        "expected equality between left and right columns");
  auto l = schema.index_of(text(left["name"], "left key name"));
  auto r = schema.index_of(text(right["name"], "right key name"));
  if (l >= left_size)
    std::swap(l, r);
  if (l >= left_size || r < left_size)
    throw std::invalid_argument("join key must refer to both inputs");
  if (schema[l].type.data_type() != schema[r].type.data_type())
    throw std::invalid_argument("join key cannot compare INT and VARCHAR");
  keys.emplace_back(l, r - left_size);
}

bool hash_mode(const Json::Value &mode) {
  if (mode == "hash")
    return true;
  if (mode == "sort")
    return false;
  throw std::invalid_argument("mode must be 'sort' or 'hash'");
}

std::vector<algebra::GroupbyOperator::Aggregate>
aggregates(const Json::Value &value) {
  using Function = algebra::GroupbyOperator::Aggregate::Function;
  array(value, "aggregates");
  std::vector<algebra::GroupbyOperator::Aggregate> result;
  for (const auto &entry : value) {
    if (!entry.isArray() || (entry.size() != 2 && entry.size() != 3))
      throw std::invalid_argument(
          "aggregate must be [function, column, optional alias]");
    auto name = text(entry[0], "aggregate function");
    auto column = text(entry[1], "aggregate column");
    Function fn;
    if (name == "count")
      fn = Function::COUNT;
    else if (name == "sum")
      fn = Function::SUM;
    else if (name == "avg")
      fn = Function::AVG;
    else if (name == "min")
      fn = Function::MIN;
    else if (name == "max")
      fn = Function::MAX;
    else
      throw std::invalid_argument("unknown aggregate function '" + name + "'");
    auto alias = entry.size() == 3
                     ? text(entry[2], "aggregate alias")
                     : name + "_" + (column == "*" ? "star" : column);
    if (entry.size() == 2)
      for (auto &ch : alias)
        if (ch == '.')
          ch = '_';
    result.push_back({fn, std::move(column), std::move(alias)});
  }
  return result;
}

struct BuildError : std::invalid_argument {
  using std::invalid_argument::invalid_argument;
};

class Builder {
public:
  Builder(storage::BufferPool &pool, const storage::Catalog &catalog)
      : pool_(pool), catalog_(catalog), frames_(pool.frames()) {}

  Node build(const Json::Value &plan, const std::string &path,
             std::size_t depth) {
    std::string label;
    try {
      if (!plan.isObject())
        throw std::invalid_argument("expected a plan node object");
      label = text(plan["op"], "op");
      const auto slot = nodes.size();
      nodes.push_back({depth, label, nullptr});
      Node impl = dispatch(plan, label, path, depth);
      std::set<std::string> seen;
      for (std::size_t i = 0; i < impl->schema().size(); ++i) {
        const auto &name = impl->schema()[i].name;
        if (!seen.insert(name).second)
          throw std::invalid_argument("duplicate column '" + name + "'");
      }
      if (label == "seqscan")
        nodes[slot].label += " " + plan["table"].asString();
      auto node = std::make_unique<OperatorWithContext>(
          std::move(impl), path + " (" + label + ")");
      nodes[slot].op = node.get();
      return node;
    } catch (const BuildError &) {
      throw;
    } catch (const std::exception &error) {
      throw BuildError(path + (label.empty() ? "" : " (" + label + ")") + ": " +
                       error.what());
    }
  }

  struct Entry {
    std::size_t depth;
    std::string label;
    const algebra::Operator *op;
  };
  std::vector<Entry> nodes;

private:
  static void fields(const Json::Value &plan,
                     std::initializer_list<const char *> allowed) {
    for (const auto &key : plan.getMemberNames()) {
      bool valid = key == "op";
      for (const char *field : allowed)
        valid = valid || key == field;
      if (!valid)
        throw std::invalid_argument("unknown field '" + key + "'");
    }
  }

  Node build_child(const Json::Value &plan, const char *key,
                   const std::string &path, std::size_t depth) {
    return build(plan[key], path + "." + key, depth + 1);
  }

  Node dispatch(const Json::Value &p, const std::string &op,
                const std::string &path, std::size_t depth) {
    if (op == "seqscan") {
      fields(p, {"table"});
      const auto table = text(p["table"], "table");
      auto info = catalog_.get_table(table);
      return std::make_unique<algebra::SequentialOperator>(
          pool_, catalog_.open_table(table), renamed(info.schema, table));
    }
    if (op == "filter") {
      fields(p, {"condition", "child"});
      auto child = build_child(p, "child", path, depth);
      auto predicate = bind(p["condition"], child->schema());
      return std::make_unique<algebra::FilterOperator>(std::move(child),
                                                       std::move(predicate));
    }
    if (op == "project") {
      fields(p, {"columns", "aliases", "child"});
      auto child = build_child(p, "child", path, depth);
      auto project = std::make_unique<algebra::ProjectionOperator>(
          std::move(child), names(p["columns"], "columns"));
      if (!p.isMember("aliases"))
        return project;
      auto aliases = names(p["aliases"], "aliases");
      if (aliases.size() != project->schema().size())
        throw std::invalid_argument(
            "aliases and columns must have the same length");
      std::vector<algebra::Column> columns;
      for (std::size_t i = 0; i < aliases.size(); ++i)
        columns.push_back({std::move(aliases[i]), project->schema()[i].type});
      return std::make_unique<Relabel>(std::move(project),
                                       algebra::Schema(std::move(columns)));
    }
    if (op == "rename") {
      fields(p, {"to", "child"});
      auto child = build_child(p, "child", path, depth);
      auto schema = renamed(child->schema(), text(p["to"], "to"));
      return std::make_unique<Relabel>(std::move(child), std::move(schema));
    }
    if (op == "limit") {
      fields(p, {"count", "child"});
      const auto &count = p["count"];
      if ((count.type() != Json::intValue && count.type() != Json::uintValue) ||
          !count.isUInt64() ||
          count.asUInt64() > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument(
            "count must be a nonnegative integer in range");
      return std::make_unique<algebra::LimitOperator>(
          build_child(p, "child", path, depth), count.asUInt64());
    }
    if (op == "sort") {
      fields(p, {"keys", "child"});
      auto child = build_child(p, "child", path, depth);
      array(p["keys"], "keys");
      std::vector<algebra::SortKey> keys;
      for (const auto &entry : p["keys"]) {
        if (!entry.isArray() || entry.size() != 2 ||
            (entry[1] != "asc" && entry[1] != "desc"))
          throw std::invalid_argument(
              "sort key must be [column, 'asc' or 'desc']");
        keys.push_back({child->schema().index_of(text(entry[0], "sort column")),
                        entry[1] == "asc"});
      }
      return std::make_unique<algebra::SortOperator>(std::move(child), pool_,
                                                     std::move(keys), frames_);
    }
    if (op == "distinct") {
      fields(p, {"mode", "child"});
      const auto mode = hash_mode(p["mode"])
                            ? algebra::DistinctOperator::Mode::HASH
                            : algebra::DistinctOperator::Mode::SORT;
      return std::make_unique<algebra::DistinctOperator>(
          build_child(p, "child", path, depth), pool_, mode,
          algebra::DistinctOperator::Options{frames_ - 1, frames_});
    }
    if (op == "group_by") {
      fields(p, {"mode", "keys", "aggregates", "child"});
      const auto mode = hash_mode(p["mode"])
                            ? algebra::GroupbyOperator::Mode::HASH
                            : algebra::GroupbyOperator::Mode::SORT;
      return std::make_unique<algebra::GroupbyOperator>(
          build_child(p, "child", path, depth), pool_, mode,
          algebra::GroupbyOperator::Options{frames_ - 1, frames_},
          names(p["keys"], "keys"), aggregates(p["aggregates"]));
    }
    if (op == "nested_loop_join" || op == "hash_join" ||
        op == "sort_merge_join") {
      fields(p, {"condition", "left", "right"});
      auto left = build_child(p, "left", path, depth);
      auto right = build_child(p, "right", path, depth);
      const auto schema =
          algebra::concat_schemas(left->schema(), right->schema());
      const auto &condition = p["condition"];
      if (op == "nested_loop_join") {
        auto predicate = bind(condition, schema);
        return std::make_unique<algebra::JoinOperator>(
            std::move(left), std::move(right), std::move(predicate), pool_,
            frames_);
      }
      std::vector<Key> keys;
      equi_keys(condition, schema, left->schema().size(), keys);
      if (op == "hash_join") {
        std::vector<algebra::HashJoinOperator::Key> bound;
        for (auto [l, r] : keys)
          bound.push_back({l, r});
        return std::make_unique<algebra::HashJoinOperator>(
            std::move(left), std::move(right), std::move(bound), pool_,
            frames_);
      }
      std::vector<algebra::SortMergeJoinOperator::Key> bound;
      for (auto [l, r] : keys)
        bound.push_back({l, r});
      return std::make_unique<algebra::SortMergeJoinOperator>(
          std::move(left), std::move(right), std::move(bound), pool_, frames_);
    }
    if (op == "union_all" || op == "union" || op == "intersect" ||
        op == "except") {
      fields(p, {"left", "right"});
      auto left = build_child(p, "left", path, depth);
      auto right = build_child(p, "right", path, depth);
      if (op == "union_all")
        return std::make_unique<algebra::UnifiedOperator>(std::move(left),
                                                          std::move(right));
      if (op == "union")
        return std::make_unique<algebra::UnionOperator>(
            std::move(left), std::move(right), pool_,
            algebra::UnionOperator::Mode::SORT,
            algebra::UnionOperator::Options{0, frames_});
      if (op == "intersect")
        return std::make_unique<algebra::IntersectOperator>(
            std::move(left), std::move(right), pool_, frames_);
      return std::make_unique<algebra::ExceptOperator>(
          std::move(left), std::move(right), pool_, frames_);
    }
    throw std::invalid_argument("unknown physical operator");
  }

  storage::BufferPool &pool_;
  const storage::Catalog &catalog_;
  std::size_t frames_;
};
} // namespace vql::query::executor

namespace vql::query {

Executor::Executor(storage::BufferPool &pool, const storage::Catalog &catalog)
    : pool_(pool), catalog_(catalog) {
  if (pool_.frames() < 4)
    throw std::invalid_argument("executor requires at least four frames");
}

ExecutionTrace
Executor::run(const Json::Value &plan,
              const std::function<void(const algebra::Schema &)> &on_schema,
              const std::function<void(const algebra::Tuple &)> &on_row) const {
  executor::Builder builder(pool_, catalog_);
  auto root = builder.build(plan, "root", 0);
  root->open();
  if (on_schema)
    on_schema(root->schema());
  while (auto row = root->next())
    if (on_row)
      on_row(*row);
  root->close();

  ExecutionTrace trace{};
  trace.totals.tuples_out = root->counters().tuples_out;
  for (const auto &entry : builder.nodes) {
    const auto count = entry.op->counters();
    trace.nodes.push_back({entry.depth, entry.label, count});
    trace.totals.comparisons += count.comparisons;
    trace.totals.page_reads += count.page_reads;
    trace.totals.page_writes += count.page_writes;
  }
  return trace;
}
} // namespace vql::query
