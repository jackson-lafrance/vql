#include "optimizer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vql::query {
namespace {

using Names = std::set<std::string>;

Json::Value unary(const std::string &op, Json::Value child) {
  Json::Value node(Json::objectValue);
  node["op"] = op;
  node["child"] = std::move(child);
  return node;
}

Json::Value and_condition(const Json::Value &left, const Json::Value &right) {
  Json::Value node(Json::objectValue);
  node["kind"] = "logical";
  node["operator"] = "and";
  node["left"] = left;
  node["right"] = right;
  return node;
}

void split_and(const Json::Value &condition, std::vector<Json::Value> &terms) {
  if (condition.isObject() && condition["kind"] == "logical" &&
      condition["operator"] == "and") {
    split_and(condition["left"], terms);
    split_and(condition["right"], terms);
  } else {
    terms.push_back(condition);
  }
}

Json::Value join_terms(const std::vector<Json::Value> &terms) {
  if (terms.empty())
    return Json::Value();
  Json::Value result = terms.front();
  for (std::size_t i = 1; i < terms.size(); ++i)
    result = and_condition(result, terms[i]);
  return result;
}

void condition_references(const Json::Value &node, Names &references) {
  if (!node.isObject())
    return;
  if (node["kind"] == "column" && node["name"].isString()) {
    const std::string name = node["name"].asString();
    const std::size_t dot = name.find('.');
    if (dot != std::string::npos)
      references.insert(name.substr(0, dot));
    return;
  }
  for (const char *key : {"left", "right", "child"})
    if (node.isMember(key))
      condition_references(node[key], references);
}

void condition_column_names(const Json::Value &node, Names &names) {
  if (!node.isObject())
    return;
  if (node["kind"] == "column" && node["name"].isString()) {
    names.insert(node["name"].asString());
    return;
  }
  for (const char *key : {"left", "right", "child"})
    if (node.isMember(key))
      condition_column_names(node[key], names);
}

Names relation_names(const Json::Value &node) {
  Names names;
  if (!node.isObject() || !node["op"].isString())
    return names;
  const std::string op = node["op"].asString();
  if (op == "scan") {
    if (node["table"].isString())
      names.insert(node["table"].asString());
    return names;
  }
  if (op == "rename") {
    if (node["to"].isString())
      names.insert(node["to"].asString());
    return names;
  }
  if (node["child"].isObject())
    names = relation_names(node["child"]);
  if (node["left"].isObject()) {
    Names right = relation_names(node["left"]);
    names.insert(right.begin(), right.end());
  }
  if (node["right"].isObject()) {
    Names right = relation_names(node["right"]);
    names.insert(right.begin(), right.end());
  }
  return names;
}

bool contains_all(const Names &available, const Names &used) {
  return std::all_of(used.begin(), used.end(), [&](const std::string &name) {
    return available.count(name) != 0;
  });
}

std::string rename_qualifier(const Json::Value &node,
                             const std::string &old_name) {
  if (!node.isObject())
    return old_name;
  if (node["op"] == "rename" && node["to"].isString())
    return node["to"].asString();
  if (node["op"] == "scan" && node["table"].isString())
    return node["table"].asString();
  return old_name;
}

void replace_qualifier(Json::Value &node, const std::string &from,
                       const std::string &to) {
  if (!node.isObject())
    return;
  if (node["kind"] == "column" && node["name"].isString()) {
    std::string name = node["name"].asString();
    const std::size_t dot = name.find('.');
    if (dot != std::string::npos && name.substr(0, dot) == from)
      node["name"] = to + name.substr(dot);
    return;
  }
  for (const char *key : {"left", "right", "child"})
    if (node.isMember(key))
      replace_qualifier(node[key], from, to);
}

std::string quote_string(const std::string &value) {
  std::string result = "'";
  for (char character : value) {
    if (character == '\'')
      result += '\'';
    result += character;
  }
  result += '\'';
  return result;
}

std::string operand_text(const Json::Value &operand) {
  if (!operand.isObject() || !operand["kind"].isString())
    throw std::invalid_argument("Physical lowering found an invalid condition operand!");
  const std::string kind = operand["kind"].asString();
  if (kind == "column" && operand["name"].isString())
    return operand["name"].asString();
  if (kind == "integer" && operand["value"].isInt())
    return std::to_string(operand["value"].asInt());
  if (kind == "varchar" && operand["value"].isString())
    return quote_string(operand["value"].asString());
  if (kind == "aggregate" && operand["name"].isString())
    return operand["name"].asString();
  throw std::invalid_argument("Physical lowering found an invalid condition operand!");
}

std::string condition_text(const Json::Value &node) {
  if (!node.isObject() || !node["kind"].isString())
    throw std::invalid_argument("Physical lowering requires structured conditions!");
  const std::string kind = node["kind"].asString();
  if (kind == "comparison") {
    if (!node["operator"].isString())
      throw std::invalid_argument("Comparison condition has no operator!");
    return operand_text(node["left"]) + " " + node["operator"].asString() + " " +
           operand_text(node["right"]);
  }
  if (kind == "not")
    return "not (" + condition_text(node["child"]) + ")";
  if (kind == "logical") {
    if (!node["operator"].isString())
      throw std::invalid_argument("Logical condition has no operator!");
    return "(" + condition_text(node["left"]) + " " +
           node["operator"].asString() + " " + condition_text(node["right"]) +
           ")";
  }
  throw std::invalid_argument("Physical lowering found an unknown condition kind!");
}

bool is_equality_condition(const Json::Value &node, const Names &left,
                           const Names &right) {
  if (!node.isObject() || !node["kind"].isString())
    return false;
  const std::string kind = node["kind"].asString();
  if (kind == "logical")
    return node["operator"] == "and" &&
           is_equality_condition(node["left"], left, right) &&
           is_equality_condition(node["right"], left, right);
  if (kind != "comparison" || node["operator"] != "=")
    return false;
  const Json::Value &a = node["left"];
  const Json::Value &b = node["right"];
  if (!a.isObject() || !b.isObject() || a["kind"] != "column" ||
      b["kind"] != "column" || !a["name"].isString() ||
      !b["name"].isString())
    return false;
  Names refs_a, refs_b;
  condition_references(a, refs_a);
  condition_references(b, refs_b);
  return (contains_all(left, refs_a) && contains_all(right, refs_b)) ||
         (contains_all(right, refs_a) && contains_all(left, refs_b));
}

std::vector<std::string> output_columns(const Json::Value &node,
                                        const storage::Catalog &catalog);
Json::Value push_selection(Json::Value child, const Json::Value &condition,
                           const storage::Catalog &catalog);

Json::Value rewrite(Json::Value node, const storage::Catalog &catalog) {
  if (!node.isObject() || !node["op"].isString())
    throw std::invalid_argument("Logical plan node must have a string op!");
  const std::string op = node["op"].asString();

  if (node["child"].isObject())
    node["child"] = rewrite(node["child"], catalog);
  if (node["left"].isObject())
    node["left"] = rewrite(node["left"], catalog);
  if (node["right"].isObject())
    node["right"] = rewrite(node["right"], catalog);

  if (op == "select") {
    if (!node["child"].isObject() || !node["condition"].isObject())
      throw std::invalid_argument("Select node requires child and condition!");
    std::vector<Json::Value> terms;
    split_and(node["condition"], terms);
    Json::Value result = node["child"];
    for (const Json::Value &term : terms)
      result = push_selection(std::move(result), term, catalog);
    return result;
  }

  if (op == "join") {
    if (!node["left"].isObject() || !node["right"].isObject() ||
        !node["condition"].isObject())
      throw std::invalid_argument("Join node requires two inputs and a condition!");
    std::vector<Json::Value> terms;
    split_and(node["condition"], terms);
    std::vector<Json::Value> remaining;
    const Names left_names = relation_names(node["left"]);
    const Names right_names = relation_names(node["right"]);
    for (const Json::Value &term : terms) {
      Names refs;
      condition_references(term, refs);
      if (!refs.empty() && contains_all(left_names, refs))
        node["left"] = push_selection(std::move(node["left"]), term, catalog);
      else if (!refs.empty() && contains_all(right_names, refs))
        node["right"] = push_selection(std::move(node["right"]), term, catalog);
      else
        remaining.push_back(term);
    }
    if (remaining.empty()) {
      node["op"] = "cross";
      node.removeMember("condition");
    } else {
      node["condition"] = join_terms(remaining);
    }
    return node;
  }

  if (op == "select" && node["child"]["op"] == "select") {
    node["condition"] = and_condition(node["condition"],
                                      node["child"]["condition"]);
    node["child"] = node["child"]["child"];
  }
  if (op == "project" && node["child"]["op"] == "project" &&
      node["columns"] == node["child"]["columns"] &&
      node["aliases"] == node["child"]["aliases"] &&
      !node.isMember("hidden_columns") &&
      !node["child"].isMember("hidden_columns"))
    node["child"] = node["child"]["child"];
  return node;
}

void remap_union_condition(Json::Value &condition,
                           const std::vector<std::string> &left_columns,
                           const std::vector<std::string> &right_columns) {
  if (!condition.isObject())
    return;
  if (condition["kind"] == "column" && condition["name"].isString()) {
    const std::string name = condition["name"].asString();
    for (std::size_t i = 0; i < left_columns.size(); ++i)
      if (left_columns[i] == name) {
        condition["name"] = right_columns[i];
        return;
      }
    return;
  }
  for (const char *key : {"left", "right", "child"})
    if (condition.isMember(key))
      remap_union_condition(condition[key], left_columns, right_columns);
}

Json::Value push_selection(Json::Value child, const Json::Value &condition,
                           const storage::Catalog &catalog) {
  if (!child.isObject() || !child["op"].isString()) {
    Json::Value select = unary("select", std::move(child));
    select["condition"] = condition;
    return select;
  }
  const std::string op = child["op"].asString();
  Names refs;
  condition_references(condition, refs);

  if (op == "rename" && child["child"].isObject() &&
      child["to"].isString()) {
    const std::string alias = child["to"].asString();
    const std::string underlying =
        rename_qualifier(child["child"], alias);
    Json::Value rewritten_condition = condition;
    replace_qualifier(rewritten_condition, alias, underlying);
    if (underlying == alias) {
      Json::Value select = unary("select", std::move(child));
      select["condition"] = condition;
      return select;
    }
    child["child"] = push_selection(std::move(child["child"]),
                                    rewritten_condition, catalog);
    return child;
  }

  if ((op == "union" || op == "union_all") &&
      child["left"].isObject() && child["right"].isObject()) {
    const std::vector<std::string> left_columns =
        output_columns(child["left"], catalog);
    const std::vector<std::string> right_columns =
        output_columns(child["right"], catalog);
    if (left_columns.size() == right_columns.size()) {
      Json::Value right_condition = condition;
      remap_union_condition(right_condition, left_columns, right_columns);
      child["left"] = push_selection(std::move(child["left"]), condition,
                                     catalog);
      child["right"] = push_selection(std::move(child["right"]),
                                      right_condition, catalog);
      return child;
    }
  }

  if (op == "group_by" && child["keys"].isArray()) {
    Names keys, referenced_columns;
    for (const Json::Value &key : child["keys"])
      if (key.isString())
        keys.insert(key.asString());
    condition_column_names(condition, referenced_columns);
    if (!referenced_columns.empty() &&
        contains_all(keys, referenced_columns)) {
      child["child"] = push_selection(std::move(child["child"]), condition,
                                      catalog);
      return child;
    }
  }

  if ((op == "join" || op == "cross") && child["left"].isObject() &&
      child["right"].isObject()) {
    const Names left_names = relation_names(child["left"]);
    const Names right_names = relation_names(child["right"]);
    const bool left_only = !refs.empty() && contains_all(left_names, refs);
    const bool right_only = !refs.empty() && contains_all(right_names, refs);
    if (left_only) {
      child["left"] = push_selection(std::move(child["left"]), condition,
                                      catalog);
      return child;
    }
    if (right_only) {
      child["right"] = push_selection(std::move(child["right"]), condition,
                                      catalog);
      return child;
    }
    if (op == "cross" && is_equality_condition(condition, left_names,
                                                right_names)) {
      child["op"] = "join";
      child["condition"] = condition;
      return child;
    }
    Names both_names = left_names;
    both_names.insert(right_names.begin(), right_names.end());
    const bool references_both_sides =
        std::any_of(refs.begin(), refs.end(), [&](const std::string &ref) {
          return left_names.count(ref) != 0;
        }) &&
        std::any_of(refs.begin(), refs.end(), [&](const std::string &ref) {
          return right_names.count(ref) != 0;
        });
    if (op == "join" && references_both_sides &&
        contains_all(both_names, refs)) {
      child["condition"] = child["condition"].isObject()
                                ? and_condition(child["condition"], condition)
                                : condition;
      return child;
    }
  }

  if (op == "select") {
    child["condition"] = and_condition(child["condition"], condition);
    return child;
  }
  Json::Value select = unary("select", std::move(child));
  select["condition"] = condition;
  return select;
}

void collect_required(const Json::Value &node, Names &required) {
  if (!node.isObject())
    return;
  if (node["condition"].isObject())
    condition_column_names(node["condition"], required);
  for (const char *field : {"columns", "hidden_columns", "keys"}) {
    const Json::Value &values = node[field];
    if (values.isArray())
      for (const Json::Value &value : values) {
        if (value.isString())
          required.insert(value.asString());
        else if (value.isArray() && !value.empty() && value[0].isString())
          required.insert(value[0].asString());
      }
  }
  if (node["aggregates"].isArray())
    for (const Json::Value &aggregate : node["aggregates"])
      if (aggregate.isArray() && aggregate.size() > 1 &&
          aggregate[1].isString() && aggregate[1].asString() != "*")
        required.insert(aggregate[1].asString());
  for (const char *key : {"child", "left", "right"})
    if (node[key].isObject())
      collect_required(node[key], required);
}

Json::Value scan_projection(const Json::Value &scan, const std::string &qualifier,
                            const storage::Catalog &catalog,
                            const Names &required) {
  if (!scan["table"].isString())
    return scan;
  const std::string table = scan["table"].asString();
  const storage::TableInfo info = catalog.get_table(table);
  Json::Value columns(Json::arrayValue);
  std::size_t selected = 0;
  for (std::size_t i = 0; i < info.schema.size(); ++i) {
    const auto &column = info.schema[i];
    const std::string requested_alias = qualifier + "." + column.name;
    const std::string requested_table = table + "." + column.name;
    if (required.count(requested_alias) != 0 ||
        required.count(requested_table) != 0) {
      columns.append(table + "." + column.name);
      ++selected;
    }
  }
  if (selected == 0 || selected == info.schema.size())
    return scan;
  Json::Value project = unary("project", scan);
  project["columns"] = std::move(columns);
  return project;
}

std::vector<std::string> output_columns(const Json::Value &node,
                                        const storage::Catalog &catalog) {
  if (!node.isObject() || !node["op"].isString())
    return {};
  const std::string op = node["op"].asString();
  if (op == "scan") {
    std::vector<std::string> result;
    const auto table = catalog.get_table(node["table"].asString());
    for (std::size_t i = 0; i < table.schema.size(); ++i)
      result.push_back(table.name + "." + table.schema[i].name);
    return result;
  }
  if (op == "rename") {
    std::vector<std::string> result = output_columns(node["child"], catalog);
    for (std::string &name : result) {
      const std::size_t dot = name.find('.');
      name = node["to"].asString() + "." +
             (dot == std::string::npos ? name : name.substr(dot + 1));
    }
    return result;
  }
  if (op == "project") {
    std::vector<std::string> result;
    for (Json::ArrayIndex i = 0; i < node["columns"].size(); ++i) {
      const Json::Value &alias = node["aliases"];
      result.push_back(alias.isArray() && i < alias.size() && alias[i].isString()
                           ? alias[i].asString()
                           : node["columns"][i].asString());
    }
    if (node["hidden_columns"].isArray())
      for (const Json::Value &column : node["hidden_columns"])
        if (column.isString())
          result.push_back(column.asString());
    return result;
  }
  if (op == "group_by") {
    std::vector<std::string> result;
    for (const Json::Value &key : node["keys"])
      if (key.isString())
        result.push_back(key.asString());
    for (const Json::Value &aggregate : node["aggregates"])
      if (aggregate.isArray() && aggregate.size() == 3 && aggregate[2].isString())
        result.push_back(aggregate[2].asString());
    return result;
  }
  if (op == "join" || op == "cross") {
    std::vector<std::string> result = output_columns(node["left"], catalog);
    std::vector<std::string> right = output_columns(node["right"], catalog);
    result.insert(result.end(), right.begin(), right.end());
    return result;
  }
  if (op == "union" || op == "union_all" || op == "intersect" || op == "except")
    return output_columns(node["left"], catalog);
  return output_columns(node["child"], catalog);
}

Json::Value remove_noop_projects(Json::Value node,
                                 const storage::Catalog &catalog) {
  if (!node.isObject() || !node["op"].isString())
    return node;
  for (const char *key : {"child", "left", "right"})
    if (node[key].isObject())
      node[key] = remove_noop_projects(node[key], catalog);
  if (node["op"] != "project" || !node["child"].isObject() ||
      (node["hidden_columns"].isArray() &&
       !node["hidden_columns"].empty()) ||
      node["aliases"].isArray())
    return node;
  const std::vector<std::string> child_columns =
      output_columns(node["child"], catalog);
  if (node["columns"].size() != child_columns.size())
    return node;
  for (Json::ArrayIndex i = 0; i < node["columns"].size(); ++i)
    if (!node["columns"][i].isString() ||
        node["columns"][i].asString() != child_columns[i])
      return node;
  return node["child"];
}

bool simple_filter_scan(const Json::Value &node) {
  if (!node.isObject() || !node["op"].isString())
    return false;
  if (node["op"] == "scan")
    return true;
  return node["op"] == "select" && simple_filter_scan(node["child"]);
}

Json::Value push_alias_projection(Json::Value node, const std::string &alias,
                                  const storage::Catalog &catalog,
                                  const Names &required) {
  if (node["op"] == "scan")
    return scan_projection(node, alias, catalog, required);
  if (node["op"] == "select")
    node["child"] = push_alias_projection(node["child"], alias, catalog,
                                           required);
  return node;
}

Json::Value push_projections(Json::Value node, const storage::Catalog &catalog,
                             const Names &required) {
  if (!node.isObject() || !node["op"].isString())
    return node;
  const std::string op = node["op"].asString();
  if (op == "rename" && node["child"].isObject() &&
      node["to"].isString() && simple_filter_scan(node["child"])) {
    node["child"] = push_alias_projection(node["child"], node["to"].asString(),
                                           catalog, required);
    return node;
  }
  if (op == "scan")
    return scan_projection(node, node["table"].asString(), catalog, required);
  for (const char *key : {"child", "left", "right"})
    if (node[key].isObject())
      node[key] = push_projections(node[key], catalog, required);
  if (op == "project") {
    node.removeMember("positions");
    node.removeMember("hidden_positions");
  }
  return node;
}

struct JoinLeaf {
  Json::Value logical;
  Json::Value physical;
  Names relations;
  RelationEstimate estimate;
};

struct JoinTerm {
  Json::Value condition;
  Names references;
};

struct Candidate {
  Json::Value logical;
  Json::Value physical;
  RelationEstimate estimate;
  std::size_t cost{0};
  Names relations;
};

void flatten_join(const Json::Value &node, std::vector<Json::Value> &leaves,
                  std::vector<Json::Value> &conditions) {
  if (node.isObject() && node["op"] == "join" &&
      node["left"].isObject() && node["right"].isObject()) {
    flatten_join(node["left"], leaves, conditions);
    flatten_join(node["right"], leaves, conditions);
    std::vector<Json::Value> terms;
    split_and(node["condition"], terms);
    conditions.insert(conditions.end(), terms.begin(), terms.end());
  } else if (node.isObject() && node["op"] == "cross" &&
             node["left"].isObject() && node["right"].isObject()) {
    flatten_join(node["left"], leaves, conditions);
    flatten_join(node["right"], leaves, conditions);
  } else {
    leaves.push_back(node);
  }
}

std::size_t estimated_size(const Json::Value &node, const char *field) {
  if (!node[field].isUInt64() && !node[field].isInt64())
    throw std::invalid_argument(std::string("Missing estimate field '") + field + "'!");
  return static_cast<std::size_t>(node[field].asUInt64());
}

Json::Value make_physical(const std::string &op, const Json::Value &left,
                          const Json::Value &right,
                          const std::string &condition) {
  Json::Value node(Json::objectValue);
  node["op"] = op;
  node["condition"] = condition;
  node["left"] = left;
  node["right"] = right;
  return node;
}

} // namespace

Optimizer::Optimizer(const storage::Catalog &catalog,
                     const StatisticsCollector &statistics,
                     std::size_t workspace_frames,
                     EstimatorFallbacks fallbacks)
    : catalog_(catalog), statistics_(statistics),
      workspace_frames_(workspace_frames), fallbacks_(fallbacks) {
  if (workspace_frames_ == 0)
    throw std::invalid_argument("Optimizer needs a nonzero frame budget!");
}

Json::Value Optimizer::optimize(const Json::Value &logical_plan) const {
  JsonPlanEstimator estimator(catalog_, statistics_, fallbacks_);
  CostModel costs(workspace_frames_);

  std::function<Json::Value(const Json::Value &)> lower;
  std::function<Json::Value(const Json::Value &)> optimize_tree;

  lower = [&](const Json::Value &node) -> Json::Value {
    if (!node.isObject() || !node["op"].isString())
      throw std::invalid_argument("Logical plan node must have a string op!");
    const std::string op = node["op"].asString();
    Json::Value result(Json::objectValue);

    if (op == "scan") {
      result["op"] = "seqscan";
      result["table"] = node["table"];
    } else if (op == "rename") {
      result = unary("rename", lower(node["child"]));
      result["to"] = node["to"];
    } else if (op == "select") {
      result = unary("filter", lower(node["child"]));
      result["condition"] = condition_text(node["condition"]);
    } else if (op == "project") {
      Json::Value columns(Json::arrayValue);
      for (const Json::Value &column : node["columns"])
        columns.append(column);
      if (node["hidden_columns"].isArray())
        for (const Json::Value &column : node["hidden_columns"])
          if (std::find(columns.begin(), columns.end(), column) == columns.end())
            columns.append(column);
      result = unary("project", lower(node["child"]));
      result["columns"] = std::move(columns);
      if (node["aliases"].isArray()) {
        Json::Value aliases = node["aliases"];
        if (node["hidden_columns"].isArray())
          for (const Json::Value &column : node["hidden_columns"])
            if (std::find(node["columns"].begin(), node["columns"].end(),
                          column) == node["columns"].end())
              aliases.append(column);
        result["aliases"] = std::move(aliases);
      }
    } else if (op == "sort") {
      Json::Value child = lower(node["child"]);
      result = unary("sort", child);
      result["keys"] = node["keys"];
      if (node["child"]["op"] == "project" &&
          node["child"]["hidden_columns"].isArray() &&
          !node["child"]["hidden_columns"].empty()) {
        Json::Value output = unary("project", result);
        output["columns"] = Json::Value(Json::arrayValue);
        for (Json::ArrayIndex i = 0; i < node["child"]["columns"].size(); ++i) {
          const Json::Value &aliases = node["child"]["aliases"];
          if (aliases.isArray() && i < aliases.size() && aliases[i].isString())
            output["columns"].append(aliases[i]);
          else
            output["columns"].append(node["child"]["columns"][i]);
        }
        result = std::move(output);
      }
    } else if (op == "limit") {
      result = unary("limit", lower(node["child"]));
      result["count"] = node["count"];
    } else if (op == "distinct") {
      const Json::Value annotated = estimator.estimate(node);
      const std::size_t pages = estimated_size(annotated, "est_pages");
      const std::size_t sort_cost = costs.sort(pages);
      const std::size_t hash_cost =
          pages == 0 ? 0 : costs.hash_join(pages, 0);
      result = unary("distinct", lower(node["child"]));
      result["mode"] = hash_cost < sort_cost ? "hash" : "sort";
    } else if (op == "group_by") {
      const Json::Value annotated = estimator.estimate(node);
      const std::size_t pages = estimated_size(annotated["child"], "est_pages");
      const std::size_t sort_cost = costs.sort(pages);
      const std::size_t hash_cost =
          pages == 0 ? 0 : costs.hash_join(pages, 0);
      result = unary("group_by", lower(node["child"]));
      result["mode"] = hash_cost < sort_cost ? "hash" : "sort";
      result["keys"] = node["keys"];
      result["aggregates"] = node["aggregates"];
    } else if (op == "union" || op == "union_all" || op == "intersect" ||
               op == "except") {
      result["op"] = op;
      result["left"] = optimize_tree(node["left"]);
      result["right"] = optimize_tree(node["right"]);
    } else if (op == "join" || op == "cross") {
      return optimize_tree(node);
    } else {
      throw std::invalid_argument("Physical lowering does not support logical operator '" +
                                  op + "'!");
    }
    return result;
  };

  optimize_tree = [&](const Json::Value &node) -> Json::Value {
    if (!node.isObject() || !node["op"].isString())
      throw std::invalid_argument("Logical plan node must have a string op!");
    if (node["op"] == "select" &&
        (node["child"]["op"] == "join" || node["child"]["op"] == "cross")) {
      std::vector<Json::Value> terms;
      split_and(node["condition"], terms);
      const Names relations = relation_names(node["child"]);
      std::vector<Json::Value> join_terms_for_plan;
      std::vector<Json::Value> residual_terms;
      for (const Json::Value &term : terms) {
        Names references;
        condition_references(term, references);
        std::size_t relation_count = 0;
        for (const std::string &reference : references)
          relation_count += relations.count(reference) != 0;
        if (contains_all(relations, references) && relation_count > 1)
          join_terms_for_plan.push_back(term);
        else
          residual_terms.push_back(term);
      }
      if (!join_terms_for_plan.empty()) {
        Json::Value logical_join = node["child"];
        if (logical_join["op"] == "cross")
          logical_join["op"] = "join";
        logical_join["condition"] = logical_join["condition"].isObject()
                                         ? and_condition(logical_join["condition"],
                                                         join_terms(join_terms_for_plan))
                                         : join_terms(join_terms_for_plan);
        Json::Value physical = optimize_tree(logical_join);
        if (residual_terms.empty())
          return physical;
        Json::Value filter = unary("filter", std::move(physical));
        filter["condition"] = condition_text(join_terms(residual_terms));
        return filter;
      }
    }
    if (node["op"] != "join" && node["op"] != "cross")
      return lower(node);

    std::vector<Json::Value> raw_leaves;
    std::vector<Json::Value> predicates;
    flatten_join(node, raw_leaves, predicates);
    std::vector<Json::Value> constant_predicates;
    predicates.erase(
        std::remove_if(predicates.begin(), predicates.end(),
                       [&](const Json::Value &predicate) {
                         Names references;
                         condition_references(predicate, references);
                         if (!references.empty())
                           return false;
                         constant_predicates.push_back(predicate);
                         return true;
                       }),
        predicates.end());
    if (raw_leaves.empty() || raw_leaves.size() > 6)
      throw std::invalid_argument("Join enumeration supports one to six relations!");

    std::vector<JoinLeaf> leaves;
    leaves.reserve(raw_leaves.size());
    for (const Json::Value &raw : raw_leaves) {
      Json::Value logical = raw;
      Json::Value annotated = estimator.estimate(logical);
      JoinLeaf leaf;
      leaf.logical = std::move(logical);
      leaf.physical = lower(leaf.logical);
      leaf.relations = relation_names(raw);
      if (leaf.relations.empty())
        leaf.relations = relation_names(leaf.logical);
      leaf.estimate.num_tuples = estimated_size(annotated, "est_rows");
      leaf.estimate.num_pages = estimated_size(annotated, "est_pages");
      const Json::Value width = annotated["est_width"];
      leaf.estimate.width = width.isNumeric() ? width.asDouble() : 0.0;
      leaves.push_back(std::move(leaf));
    }

    const std::size_t count = leaves.size();
    const std::size_t state_count = std::size_t{1} << count;
    std::vector<std::optional<Candidate>> best(state_count);
    for (std::size_t i = 0; i < count; ++i) {
      const std::size_t mask = std::size_t{1} << i;
      Candidate base;
      base.logical = leaves[i].logical;
      base.physical = leaves[i].physical;
      base.estimate = leaves[i].estimate;
      base.cost = costs.seqscan(base.estimate.num_pages);
      base.relations = leaves[i].relations;
      best[mask] = std::move(base);
    }

    for (std::size_t size = 2; size <= count; ++size) {
      for (std::size_t mask = 1; mask < state_count; ++mask) {
        if (static_cast<std::size_t>(__builtin_popcountll(mask)) != size)
          continue;
        std::optional<Candidate> chosen;
        auto consider = [&](std::size_t left_mask, std::size_t right_index,
                            bool allow_cross) {
          if (!best[left_mask])
            return;
          Names combined = best[left_mask]->relations;
          combined.insert(leaves[right_index].relations.begin(),
                          leaves[right_index].relations.end());
          std::vector<Json::Value> crossing;
          for (const Json::Value &predicate : predicates) {
            Names refs;
            condition_references(predicate, refs);
            if (!refs.empty() && contains_all(combined, refs) &&
                !contains_all(best[left_mask]->relations, refs) &&
                !contains_all(leaves[right_index].relations, refs))
              crossing.push_back(predicate);
          }
          if (crossing.empty() && !allow_cross)
            return;
          Json::Value condition = join_terms(crossing);
          Json::Value logical(Json::objectValue);
          logical["op"] = crossing.empty() ? "cross" : "join";
          logical["left"] = best[left_mask]->logical;
          logical["right"] = leaves[right_index].logical;
          if (!crossing.empty())
            logical["condition"] = condition;
          Json::Value annotated;
          try {
            annotated = estimator.estimate(logical);
          } catch (const std::exception &) {
            return;
          }
          RelationEstimate estimate{estimated_size(annotated, "est_rows"),
                                    estimated_size(annotated, "est_pages"),
                                    annotated["est_width"].asDouble()};

          const Names left_relations = best[left_mask]->relations;
          const Names right_relations = leaves[right_index].relations;
          const bool equality = !crossing.empty() &&
              std::all_of(crossing.begin(), crossing.end(), [&](const auto &p) {
                return is_equality_condition(p, left_relations, right_relations);
              });
          struct Choice { std::size_t cost; std::string op; };
          std::vector<Choice> choices;
          try {
            choices.push_back({costs.nested_loop_join(
                                   best[left_mask]->estimate.num_pages,
                                   leaves[right_index].estimate.num_pages),
                               "nested_loop_join"});
            if (equality) {
              choices.push_back({costs.hash_join(
                                     best[left_mask]->estimate.num_pages,
                                     leaves[right_index].estimate.num_pages),
                                 "hash_join"});
              choices.push_back({costs.sort_merge_join(
                                     best[left_mask]->estimate.num_pages,
                                     leaves[right_index].estimate.num_pages),
                                 "sort_merge_join"});
            }
          } catch (const std::invalid_argument &) {
            return;
          }
          for (const Choice &choice : choices) {
            const std::size_t prefix_cost = size == 2 ? 0 : best[left_mask]->cost;
            if (choice.cost > std::numeric_limits<std::size_t>::max() - prefix_cost)
              continue;
            Candidate candidate;
            candidate.logical = logical;
            candidate.estimate = estimate;
            candidate.cost = prefix_cost + choice.cost;
            candidate.relations = combined;
            candidate.physical = make_physical(
                choice.op, best[left_mask]->physical,
                leaves[right_index].physical,
                crossing.empty() ? "1 = 1" : condition_text(condition));
            if (!chosen || candidate.cost < chosen->cost)
              chosen = std::move(candidate);
          }
        };

        for (std::size_t right_index = 0; right_index < count; ++right_index) {
          const std::size_t right_mask = std::size_t{1} << right_index;
          if ((mask & right_mask) == 0)
            continue;
          const std::size_t left_mask = mask ^ right_mask;
          if (left_mask != 0)
            consider(left_mask, right_index, false);
        }
        if (!chosen) {
          for (std::size_t right_index = 0; right_index < count; ++right_index) {
            const std::size_t right_mask = std::size_t{1} << right_index;
            if ((mask & right_mask) == 0)
              continue;
            const std::size_t left_mask = mask ^ right_mask;
            if (left_mask != 0)
              consider(left_mask, right_index, true);
          }
        }
        best[mask] = std::move(chosen);
      }
    }

    const std::size_t full = state_count - 1;
    if (!best[full])
      throw std::invalid_argument("No executable join plan could be estimated!");
    Json::Value physical = best[full]->physical;
    if (!constant_predicates.empty()) {
      Json::Value filter = unary("filter", std::move(physical));
      filter["condition"] = condition_text(join_terms(constant_predicates));
      physical = std::move(filter);
    }
    return physical;
  };

  Json::Value rewritten =
      remove_noop_projects(rewrite(logical_plan, catalog_), catalog_);
  Names required;
  collect_required(rewritten, required);
  rewritten = push_projections(std::move(rewritten), catalog_, required);
  rewritten = remove_noop_projects(std::move(rewritten), catalog_);
  (void)estimator.estimate(rewritten);
  return optimize_tree(rewritten);
}

} // namespace vql::query
