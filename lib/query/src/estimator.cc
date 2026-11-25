#include "estimator.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace vql::query {
namespace {

std::size_t checked_ceil(long double value, const char *message) {
  if (!std::isfinite(value) || value > std::numeric_limits<std::size_t>::max())
    throw std::overflow_error(message);
  return static_cast<std::size_t>(std::ceil(value));
}

bool contains(const HistogramBucket &bucket, const StatisticValue &value) {
  return bucket.minimum.index() == value.index() &&
         !(value < bucket.minimum) && !(bucket.maximum < value);
}

StatisticValue statistic_value(const Literal &literal) {
  if (const auto *integer = std::get_if<std::int32_t>(&literal.value))
    return *integer;
  return std::get<std::string>(literal.value);
}

ComparisonOperator reverse(ComparisonOperator op) {
  switch (op) {
  case ComparisonOperator::LESS_THAN:
    return ComparisonOperator::GREATER_THAN;
  case ComparisonOperator::LESS_THAN_OR_EQUAL:
    return ComparisonOperator::GREATER_THAN_OR_EQUAL;
  case ComparisonOperator::GREATER_THAN:
    return ComparisonOperator::LESS_THAN;
  case ComparisonOperator::GREATER_THAN_OR_EQUAL:
    return ComparisonOperator::LESS_THAN_OR_EQUAL;
  default:
    return op;
  }
}

const ColumnStatisticsBinding &find_binding(
    const ColumnRef &column,
    const std::vector<ColumnStatisticsBinding> &bindings) {
  if (!column.qualifier || !column.resolved_position)
    throw std::invalid_argument(
        "Estimator requires semantically resolved columns!");
  for (const auto &binding : bindings)
    if (binding.qualifier == column.qualifier->name &&
        binding.position == *column.resolved_position)
      return binding;
  throw std::invalid_argument("No statistics binding for column '" +
                              column.qualifier->name + "." + column.name.name +
                              "'!");
}

ComparisonOperator comparison_operator(const Json::Value &value) {
  if (!value.isString())
    throw std::invalid_argument("Condition operator must be a string!");
  const std::string op = value.asString();
  if (op == "=")
    return ComparisonOperator::EQUAL;
  if (op == "<>")
    return ComparisonOperator::NOT_EQUAL;
  if (op == "<")
    return ComparisonOperator::LESS_THAN;
  if (op == "<=")
    return ComparisonOperator::LESS_THAN_OR_EQUAL;
  if (op == ">")
    return ComparisonOperator::GREATER_THAN;
  if (op == ">=")
    return ComparisonOperator::GREATER_THAN_OR_EQUAL;
  throw std::invalid_argument("Unknown condition comparison operator!");
}

Condition condition_from_json(
    const Json::Value &node,
    const std::vector<ColumnStatisticsBinding> &bindings) {
  if (!node.isObject() || !node["kind"].isString())
    throw std::invalid_argument("Condition node must have a string kind!");
  const std::string kind = node["kind"].asString();
  if (kind == "not") {
    if (!node.isMember("child"))
      throw std::invalid_argument("NOT condition is missing its child!");
    return Condition{NotCondition{
                         std::make_unique<Condition>(
                             condition_from_json(node["child"], bindings)),
                         {}},
                     {}};
  }
  if (kind == "logical") {
    if (!node["operator"].isString() || !node.isMember("left") ||
        !node.isMember("right"))
      throw std::invalid_argument("Logical condition is incomplete!");
    const std::string op = node["operator"].asString();
    LogicalOperator logical_op;
    if (op == "and")
      logical_op = LogicalOperator::AND;
    else if (op == "or")
      logical_op = LogicalOperator::OR;
    else
      throw std::invalid_argument("Unknown logical condition operator!");
    return Condition{LogicalCondition{
                         logical_op,
                         std::make_unique<Condition>(
                             condition_from_json(node["left"], bindings)),
                         std::make_unique<Condition>(
                             condition_from_json(node["right"], bindings)),
                         {}},
                     {}};
  }
  if (kind != "comparison" || !node.isMember("left") ||
      !node.isMember("right"))
    throw std::invalid_argument("Unknown or incomplete condition node!");

  auto operand_from_json = [&](const Json::Value &operand) -> Operand {
    if (!operand.isObject() || !operand["kind"].isString())
      throw std::invalid_argument("Condition operand must have a kind!");
    const std::string operand_kind = operand["kind"].asString();
    if (operand_kind == "integer") {
      const Json::Value &value = operand["value"];
      if (!value.isInt())
        throw std::invalid_argument("Integer condition literal is invalid!");
      return Literal{value.asInt(), {}};
    }
    if (operand_kind == "varchar") {
      if (!operand["value"].isString())
        throw std::invalid_argument("VARCHAR condition literal is invalid!");
      return Literal{operand["value"].asString(), {}};
    }
    if (operand_kind == "aggregate") {
      if (!operand["name"].isString())
        throw std::invalid_argument("Aggregate condition name is invalid!");
      return AggregateCall{AggregateFunction::COUNT, Star{}, {}, std::nullopt,
                           operand["name"].asString()};
    }
    if (operand_kind != "column" || !operand["name"].isString())
      throw std::invalid_argument("Unknown condition operand kind!");

    const std::string name = operand["name"].asString();
    const std::size_t dot = name.find('.');
    if (dot == std::string::npos)
      throw std::invalid_argument("Column condition name must be qualified!");
    const std::string qualifier = name.substr(0, dot);
    const std::string column_name = name.substr(dot + 1);
    for (const auto &binding : bindings)
      if (binding.qualifier == qualifier &&
          binding.column_name == column_name) {
        return ColumnRef{NameWithSpan{qualifier, {}},
                         NameWithSpan{column_name, {}}, {}, std::nullopt,
                         binding.position};
      }
    throw std::invalid_argument("No statistics binding for condition column '" +
                                name + "'!");
  };

  Comparison comparison{operand_from_json(node["left"]),
                        comparison_operator(node["operator"]),
                        operand_from_json(node["right"]), {}};
  return Condition{std::move(comparison), {}};
}

struct EstimatedPlan {
  Json::Value json;
  RelationEstimate relation;
  std::vector<ColumnStatisticsBinding> columns;
};

std::size_t plan_product(std::size_t left, std::size_t right) {
  return checked_ceil(static_cast<long double>(left) * right,
                      "Plan cardinality estimate overflow!");
}

void add_estimate_fields(Json::Value &node, const RelationEstimate &estimate) {
  node["est_rows"] = Json::UInt64(estimate.num_tuples);
  node["est_pages"] = Json::UInt64(estimate.num_pages);
  node["est_width"] = estimate.width;
}

bool compare_values(const StatisticValue &left, const StatisticValue &right,
                    ComparisonOperator op) {
  if (left.index() != right.index())
    throw std::invalid_argument("Cannot estimate a comparison of unlike types!");
  switch (op) {
  case ComparisonOperator::EQUAL:
    return left == right;
  case ComparisonOperator::NOT_EQUAL:
    return left != right;
  case ComparisonOperator::LESS_THAN:
    return left < right;
  case ComparisonOperator::LESS_THAN_OR_EQUAL:
    return !(right < left);
  case ComparisonOperator::GREATER_THAN:
    return right < left;
  case ComparisonOperator::GREATER_THAN_OR_EQUAL:
    return !(left < right);
  }
  throw std::logic_error("Unknown comparison operator in estimator!");
}

} // namespace

std::size_t CardinalityEstimator::distinct_values(
    std::size_t rows, std::optional<std::size_t> observed_ndv) const {
  if (rows == 0)
    return 0;
  if (observed_ndv && *observed_ndv != 0)
    return std::min(rows, *observed_ndv);
  return static_cast<std::size_t>(std::ceil(std::sqrt(
      static_cast<long double>(rows))));
}

double CardinalityEstimator::equality_selectivity(
    std::size_t rows, const ColumnStatistics *statistics,
    const StatisticValue &value) const {
  if (rows == 0)
    return 0;

  if (statistics && statistics->num_values != 0) {
    if (!statistics->minimum || !statistics->maximum ||
        value.index() != statistics->minimum->index() ||
        value.index() != statistics->maximum->index() ||
        value < *statistics->minimum || *statistics->maximum < value)
      return 0;

    if (!statistics->histogram.empty()) {
      long double selectivity = 0;
      for (const HistogramBucket &bucket : statistics->histogram) {
        if (bucket.ndv != 0 && contains(bucket, value))
          selectivity += static_cast<long double>(bucket.num_tuples) /
                         (static_cast<long double>(rows) * bucket.ndv);
      }
      return static_cast<double>(std::min(1.0L, selectivity));
    }

    const std::size_t ndv = distinct_values(rows, statistics->ndv);
    return ndv == 0 ? 0 : 1.0 / static_cast<double>(ndv);
  }

  const std::size_t ndv = distinct_values(rows);
  return ndv == 0 ? 0 : 1.0 / static_cast<double>(ndv);
}

std::size_t CardinalityEstimator::equality_join_rows(
    std::size_t left_rows, const ColumnStatistics *left,
    std::size_t right_rows, const ColumnStatistics *right) const {
  if (left_rows == 0 || right_rows == 0)
    return 0;

  const std::size_t left_ndv =
      distinct_values(left_rows, left ? std::optional(left->ndv) : std::nullopt);
  const std::size_t right_ndv = distinct_values(
      right_rows, right ? std::optional(right->ndv) : std::nullopt);
  const std::size_t denominator = std::max(left_ndv, right_ndv);
  const long double rows = static_cast<long double>(left_rows) * right_rows /
                           static_cast<long double>(denominator);
  return std::max<std::size_t>(1,
                               checked_ceil(rows, "Join row estimate overflow!"));
}

std::size_t CardinalityEstimator::output_rows(std::size_t input_rows,
                                              double selectivity) const {
  if (!std::isfinite(selectivity) || selectivity < 0 || selectivity > 1)
    throw std::invalid_argument("Selectivity must be between zero and one!");
  if (input_rows == 0)
    return 0;
  return std::max<std::size_t>(
      1, checked_ceil(static_cast<long double>(input_rows) * selectivity,
                      "Output row estimate overflow!"));
}

double CardinalityEstimator::and_selectivity(double left,
                                             double right) const {
  if (!std::isfinite(left) || !std::isfinite(right) || left < 0 || left > 1 ||
      right < 0 || right > 1)
    throw std::invalid_argument("Selectivities must be between zero and one!");
  return left * right;
}

double CardinalityEstimator::or_selectivity(double left,
                                            double right) const {
  if (!std::isfinite(left) || !std::isfinite(right) || left < 0 || left > 1 ||
      right < 0 || right > 1)
    throw std::invalid_argument("Selectivities must be between zero and one!");
  return left + right - left * right;
}

double CardinalityEstimator::not_selectivity(double input) const {
  if (!std::isfinite(input) || input < 0 || input > 1)
    throw std::invalid_argument("Selectivity must be between zero and one!");
  return 1 - input;
}

double CardinalityEstimator::condition_selectivity(
    const Condition &condition,
    const std::vector<ColumnStatisticsBinding> &columns,
    const EstimatorFallbacks &fallbacks) const {
  if (!std::isfinite(fallbacks.unknown_range_selectivity) ||
      fallbacks.unknown_range_selectivity < 0 ||
      fallbacks.unknown_range_selectivity > 1 ||
      !std::isfinite(fallbacks.varchar_bucket_fraction) ||
      fallbacks.varchar_bucket_fraction < 0 ||
      fallbacks.varchar_bucket_fraction > 1 ||
      !std::isfinite(fallbacks.set_overlap_fraction) ||
      fallbacks.set_overlap_fraction < 0 || fallbacks.set_overlap_fraction > 1)
    throw std::invalid_argument("Estimator fallbacks must be between zero and one!");
  std::function<double(const Condition &)> estimate =
      [&](const Condition &node) -> double {
    if (const auto *negation = std::get_if<NotCondition>(&node.value)) {
      if (!negation->child)
        throw std::invalid_argument("Cannot estimate an empty NOT condition!");
      return not_selectivity(estimate(*negation->child));
    }
    if (const auto *logical = std::get_if<LogicalCondition>(&node.value)) {
      if (!logical->left || !logical->right)
        throw std::invalid_argument("Cannot estimate an incomplete condition!");
      const double left = estimate(*logical->left);
      const double right = estimate(*logical->right);
      return logical->op == LogicalOperator::AND
                 ? and_selectivity(left, right)
                 : or_selectivity(left, right);
    }

    const auto &comparison = std::get<Comparison>(node.value);
    const auto *left_column = std::get_if<ColumnRef>(&comparison.left);
    const auto *right_column = std::get_if<ColumnRef>(&comparison.right);
    const auto *left_literal = std::get_if<Literal>(&comparison.left);
    const auto *right_literal = std::get_if<Literal>(&comparison.right);

    if (left_column && right_literal) {
      const auto &binding = find_binding(*left_column, columns);
      if (binding.relation_rows == 0)
        return 0;
      const auto value = statistic_value(*right_literal);
      if (comparison.op == ComparisonOperator::EQUAL)
        return equality_selectivity(binding.relation_rows, binding.statistics,
                                    value);
      if (comparison.op == ComparisonOperator::NOT_EQUAL)
        return 1 - equality_selectivity(binding.relation_rows,
                                        binding.statistics, value);

      const auto *stats = binding.statistics;
      if (!stats || !stats->minimum || !stats->maximum ||
          stats->num_values == 0)
        return fallbacks.unknown_range_selectivity;
      if (value.index() != stats->minimum->index())
        throw std::invalid_argument("Predicate literal type does not match column!");
      if (value < *stats->minimum)
        return comparison.op == ComparisonOperator::GREATER_THAN ||
                       comparison.op == ComparisonOperator::GREATER_THAN_OR_EQUAL
                   ? 1.0
                   : 0.0;
      if (*stats->maximum < value)
        return comparison.op == ComparisonOperator::GREATER_THAN ||
                       comparison.op == ComparisonOperator::GREATER_THAN_OR_EQUAL
                   ? 0.0
                   : 1.0;

      std::vector<HistogramBucket> fallback;
      const std::vector<HistogramBucket> *histogram = &stats->histogram;
      if (histogram->empty()) {
        fallback.push_back({*stats->minimum, *stats->maximum,
                            binding.relation_rows,
                            distinct_values(binding.relation_rows, stats->ndv)});
        histogram = &fallback;
      }
      long double less_equal = 0;
      for (const HistogramBucket &bucket : *histogram) {
        if (value < bucket.minimum)
          continue;
        if (!(bucket.maximum < value)) {
          if (bucket.minimum == bucket.maximum) {
            if (!(value < bucket.minimum))
              less_equal += bucket.num_tuples;
          } else if (const auto *low = std::get_if<std::int32_t>(&bucket.minimum)) {
            const auto high = std::get<std::int32_t>(bucket.maximum);
            const auto point = std::get<std::int32_t>(value);
            const long double fraction =
                (static_cast<long double>(point) - *low) /
                (static_cast<long double>(high) - *low);
            less_equal += bucket.num_tuples * std::clamp(fraction, 0.0L, 1.0L);
          } else {
            // VARCHAR values have lexical order but no numeric distance.
            less_equal += static_cast<long double>(bucket.num_tuples) *
                          fallbacks.varchar_bucket_fraction;
          }
        } else {
          less_equal += bucket.num_tuples;
        }
      }
      less_equal /= static_cast<long double>(binding.relation_rows);
      less_equal = std::clamp(less_equal, 0.0L, 1.0L);
      const double equal = equality_selectivity(binding.relation_rows, stats,
                                                value);
      const double less = std::clamp(static_cast<double>(less_equal) - equal,
                                     0.0, 1.0);
      const double less_or_equal = std::min(1.0, less + equal);
      switch (comparison.op) {
      case ComparisonOperator::LESS_THAN:
        return less;
      case ComparisonOperator::LESS_THAN_OR_EQUAL:
        return less_or_equal;
      case ComparisonOperator::GREATER_THAN:
        return 1 - less_or_equal;
      case ComparisonOperator::GREATER_THAN_OR_EQUAL:
        return 1 - less;
      default:
        throw std::logic_error("Unexpected range comparison operator!");
      }
    }

    if (left_literal && right_column) {
      Comparison reversed{comparison.right, reverse(comparison.op),
                          comparison.left, comparison.operator_span};
      Condition temporary{std::move(reversed), node.span};
      return estimate(temporary);
    }

    if (left_column && right_column) {
      const auto &left = find_binding(*left_column, columns);
      const auto &right = find_binding(*right_column, columns);
      if (left.relation_rows == 0 || right.relation_rows == 0)
        return 0;
      if (comparison.op == ComparisonOperator::EQUAL) {
        if (left.qualifier == right.qualifier &&
            left.position == right.position)
          return 1;
        const std::size_t left_ndv =
            distinct_values(left.relation_rows,
                            left.statistics
                                ? std::optional(left.statistics->ndv)
                                : std::nullopt);
        const std::size_t right_ndv =
            distinct_values(right.relation_rows,
                            right.statistics
                                ? std::optional(right.statistics->ndv)
                                : std::nullopt);
        return 1.0 / static_cast<double>(std::max(left_ndv, right_ndv));
      }
      if (comparison.op == ComparisonOperator::NOT_EQUAL)
        return 1 - condition_selectivity(
                       Condition{Comparison{comparison.left,
                                            ComparisonOperator::EQUAL,
                                            comparison.right,
                                            comparison.operator_span},
                                 node.span},
                       columns, fallbacks);
      return 0.1;
    }

    if (left_literal && right_literal) {
      const bool matches = compare_values(statistic_value(*left_literal),
                                          statistic_value(*right_literal),
                                          comparison.op);
      return matches ? 1.0 : 0.0;
    }

    // Aggregate comparisons have no distribution statistics in Module 4.
    return 0.1;
  };
  return estimate(condition);
}

std::size_t CardinalityEstimator::output_pages(std::size_t rows,
                                               double width) const {
  if (!std::isfinite(width) || width < 0)
    throw std::invalid_argument("Estimated row width cannot be negative!");
  if (rows == 0 || width == 0)
    return 0;
  return checked_ceil(static_cast<long double>(rows) * width / 4096.0L,
                      "Output page estimate overflow!");
}

RelationEstimate CardinalityEstimator::filter(const RelationEstimate &input,
                                              double selectivity) const {
  RelationEstimate output;
  output.num_tuples = output_rows(input.num_tuples, selectivity);
  output.width = input.width;
  output.num_pages = output_pages(output.num_tuples, output.width);
  return output;
}

JsonPlanEstimator::JsonPlanEstimator(
    const storage::Catalog &catalog, const StatisticsCollector &statistics,
    EstimatorFallbacks fallbacks)
    : catalog_(catalog), statistics_(statistics), fallbacks_(fallbacks) {
  if (!std::isfinite(fallbacks_.unknown_range_selectivity) ||
      fallbacks_.unknown_range_selectivity < 0 ||
      fallbacks_.unknown_range_selectivity > 1 ||
      !std::isfinite(fallbacks_.varchar_bucket_fraction) ||
      fallbacks_.varchar_bucket_fraction < 0 ||
      fallbacks_.varchar_bucket_fraction > 1 ||
      !std::isfinite(fallbacks_.set_overlap_fraction) ||
      fallbacks_.set_overlap_fraction < 0 || fallbacks_.set_overlap_fraction > 1)
    throw std::invalid_argument("Invalid estimator fallback values!");
}

Json::Value JsonPlanEstimator::estimate(const Json::Value &logical_plan) const {
  CardinalityEstimator estimator;
  std::map<std::string, TableStatistics> cached_statistics;

  std::function<EstimatedPlan(const Json::Value &)> visit =
      [&](const Json::Value &input) -> EstimatedPlan {
    if (!input.isObject() || !input["op"].isString())
      throw std::invalid_argument("Logical plan node must have a string op!");
    const std::string op = input["op"].asString();
    Json::Value node = input;
    EstimatedPlan result;

    if (op == "scan") {
      if (!input["table"].isString())
        throw std::invalid_argument("Scan node must name a table!");
      const std::string table = input["table"].asString();
      const storage::TableInfo info = catalog_.get_table(table);
      const TableStatistics *saved = nullptr;
      auto cached = cached_statistics.find(table);
      if (cached != cached_statistics.end()) {
        saved = &cached->second;
      } else if (auto loaded = statistics_.load(table)) {
        auto inserted = cached_statistics.emplace(table, std::move(*loaded));
        saved = &inserted.first->second;
      }

      result.relation.num_tuples = saved ? saved->num_tuples : info.num_tuples;
      if (saved) {
        result.relation.num_pages = saved->num_pages;
        result.relation.width = saved->width();
      } else {
        // Heap file pages include linked directory pages. Each directory page
        // holds 681 data-page entries; P=N+ceil(N/681), so D=ceil(P/682).
        const std::size_t total_pages = info.num_pages;
        const std::size_t directory_pages =
            total_pages / 682 + (total_pages % 682 != 0);
        result.relation.num_pages = total_pages - directory_pages;
      }
      double maximum_schema_width = 0;
      for (std::size_t position = 0; position < info.schema.size(); ++position) {
        const auto &column = info.schema[position];
        const double width = column.type.data_type() == algebra::DataType::INT
                                 ? 4.0
                                 : static_cast<double>(column.type.max_length() + 2);
        maximum_schema_width += width;
        const ColumnStatistics *column_stats =
            saved && position < saved->columns.size()
                ? &saved->columns[position]
                : nullptr;
        result.columns.push_back({table, column.name, position,
                                  result.relation.num_tuples, width,
                                  column_stats});
      }
      if (!saved)
        result.relation.width = maximum_schema_width;
    } else if (op == "rename") {
      if (!input["child"].isObject() || !input["to"].isString())
        throw std::invalid_argument("Rename node requires child and target!");
      result = visit(input["child"]);
      node["child"] = result.json;
      const std::string target = input["to"].asString();
      for (auto &column : result.columns)
        column.qualifier = target;
    } else if (op == "select") {
      if (!input["child"].isObject() || !input["condition"].isObject())
        throw std::invalid_argument(
            "Select node requires child and structured condition!");
      result = visit(input["child"]);
      Condition condition = condition_from_json(input["condition"],
                                                result.columns);
      const double selectivity = estimator.condition_selectivity(
          condition, result.columns, fallbacks_);
      result.relation = estimator.filter(result.relation, selectivity);
      node["child"] = result.json;
    } else if (op == "join" || op == "cross") {
      if (!input["left"].isObject() || !input["right"].isObject())
        throw std::invalid_argument("Binary plan node requires left and right!");
      EstimatedPlan left = visit(input["left"]);
      EstimatedPlan right = visit(input["right"]);
      result.columns = std::move(left.columns);
      result.columns.insert(result.columns.end(), right.columns.begin(),
                            right.columns.end());
      result.relation.width = left.relation.width + right.relation.width;
      const std::size_t product =
          plan_product(left.relation.num_tuples, right.relation.num_tuples);
      if (op == "join") {
        if (!input["condition"].isObject())
          throw std::invalid_argument(
              "Join node requires a structured condition!");
        Condition condition = condition_from_json(input["condition"],
                                                  result.columns);
        const double selectivity = estimator.condition_selectivity(
            condition, result.columns, fallbacks_);
        result.relation.num_tuples = estimator.output_rows(product, selectivity);
      } else {
        result.relation.num_tuples = product;
      }
      result.relation.num_pages = estimator.output_pages(
          result.relation.num_tuples, result.relation.width);
      node["left"] = left.json;
      node["right"] = right.json;
    } else if (op == "union" || op == "union_all" || op == "intersect" ||
               op == "except") {
      if (!input["left"].isObject() || !input["right"].isObject())
        throw std::invalid_argument("Set-operation node requires two inputs!");
      EstimatedPlan left = visit(input["left"]);
      EstimatedPlan right = visit(input["right"]);
      result.relation.width = left.relation.width;
      result.columns = std::move(left.columns);
      const std::size_t smaller =
          std::min(left.relation.num_tuples, right.relation.num_tuples);
      const std::size_t overlap = checked_ceil(
          static_cast<long double>(smaller) * fallbacks_.set_overlap_fraction,
          "Set-operation estimate overflow!");
      const long double combined =
          static_cast<long double>(left.relation.num_tuples) +
          right.relation.num_tuples;
      const std::size_t sum =
          checked_ceil(combined, "Set-operation estimate overflow!");
      if (op == "union_all")
        result.relation.num_tuples = sum;
      else if (op == "union")
        result.relation.num_tuples = sum - overlap;
      else if (op == "intersect")
        result.relation.num_tuples = overlap;
      else
        result.relation.num_tuples = left.relation.num_tuples - overlap;
      result.relation.num_pages = estimator.output_pages(
          result.relation.num_tuples, result.relation.width);
      node["left"] = left.json;
      node["right"] = right.json;
    } else if (op == "project") {
      if (!input["child"].isObject() || !input["columns"].isArray())
        throw std::invalid_argument("Project node requires child and columns!");
      result = visit(input["child"]);
      node["child"] = result.json;
      const auto &sources = input["columns"];
      const auto &positions = input["positions"];
      const auto &aliases = input["aliases"];
      const double input_width = result.relation.width;
      result.relation.width = 0;
      const std::vector<ColumnStatisticsBinding> input_columns = result.columns;
      std::vector<std::size_t> projected_positions;
      std::vector<ColumnStatisticsBinding> projected_columns;
      auto add_projected = [&](const Json::Value &name, std::size_t index,
                               const Json::Value *position_list,
                               const Json::Value *output_names) {
        if (!name.isString())
          throw std::invalid_argument("Projected column name must be a string!");
        std::size_t position = input_columns.size();
        if (position_list && position_list->isArray()) {
          if (index >= position_list->size() ||
              !(*position_list)[static_cast<Json::ArrayIndex>(index)].isUInt64())
            throw std::invalid_argument("Projected column position is invalid!");
          position = static_cast<std::size_t>(
              (*position_list)[static_cast<Json::ArrayIndex>(index)].asUInt64());
        } else {
          for (std::size_t i = 0; i < input_columns.size(); ++i) {
            const auto &candidate = input_columns[i];
            if (candidate.qualifier + "." + candidate.column_name ==
                name.asString()) {
              position = i;
              break;
            }
          }
        }
        if (position >= input_columns.size())
          throw std::invalid_argument("Projected column position is out of range!");
        projected_positions.push_back(position);
        ColumnStatisticsBinding binding = input_columns[position];
        result.relation.width += binding.width;
        if (output_names && output_names->isArray() &&
            index < output_names->size() &&
            (*output_names)[static_cast<Json::ArrayIndex>(index)].isString()) {
          const std::string output =
              (*output_names)[static_cast<Json::ArrayIndex>(index)].asString();
          const std::size_t dot = output.find('.');
          binding.qualifier = dot == std::string::npos ? "" : output.substr(0, dot);
          binding.column_name = dot == std::string::npos ? output
                                                         : output.substr(dot + 1);
        }
        binding.position = projected_columns.size();
        projected_columns.push_back(std::move(binding));
      };
      result.columns.clear();
      for (Json::ArrayIndex i = 0; i < sources.size(); ++i)
        add_projected(sources[i], i, positions.isArray() ? &positions : nullptr,
                      aliases.isArray() ? &aliases : nullptr);
      const auto &hidden = input["hidden_columns"];
      const auto &hidden_positions = input["hidden_positions"];
      if (hidden.isArray())
        for (Json::ArrayIndex i = 0; i < hidden.size(); ++i)
          add_projected(hidden[i], i,
                        hidden_positions.isArray() ? &hidden_positions : nullptr,
                        nullptr);
      result.columns = std::move(projected_columns);
      if (projected_positions.size() == input_columns.size()) {
        std::vector<bool> seen(input_columns.size(), false);
        bool keeps_all = true;
        for (std::size_t position : projected_positions) {
          if (seen[position]) {
            keeps_all = false;
            break;
          }
          seen[position] = true;
        }
        if (keeps_all)
          result.relation.width = input_width;
      }
      result.relation.num_pages = estimator.output_pages(
          result.relation.num_tuples, result.relation.width);
    } else if (op == "group_by") {
      if (!input["child"].isObject() || !input["keys"].isArray() ||
          !input["aggregates"].isArray())
        throw std::invalid_argument(
            "Group-by node requires child, keys, and aggregates!");
      EstimatedPlan child = visit(input["child"]);
      node["child"] = child.json;
      result.relation.width = 0;
      std::size_t groups = child.relation.num_tuples == 0 ? 0 : 1;
      for (Json::ArrayIndex i = 0; i < input["keys"].size(); ++i) {
        const Json::Value &key = input["keys"][i];
        if (!key.isString())
          throw std::invalid_argument("Group-by key must be a string!");
        auto found = std::find_if(
            child.columns.begin(), child.columns.end(), [&](const auto &column) {
              return column.qualifier + "." + column.column_name ==
                     key.asString();
            });
        if (found == child.columns.end())
          throw std::invalid_argument("Group-by key has no child column!");
        if (child.relation.num_tuples != 0) {
          const std::size_t ndv = estimator.distinct_values(
              found->relation_rows,
              found->statistics ? std::optional(found->statistics->ndv)
                                : std::nullopt);
          groups = ndv != 0 && groups > child.relation.num_tuples / ndv
                       ? child.relation.num_tuples
                       : std::min(child.relation.num_tuples, groups * ndv);
        }
        result.relation.width += found->width;
        ColumnStatisticsBinding output = *found;
        output.position = result.columns.size();
        output.relation_rows = groups;
        result.columns.push_back(std::move(output));
      }
      if (input["keys"].empty())
        groups = 1; // A global aggregate emits one row, even for empty input.
      result.relation.num_tuples = groups;
      for (Json::ArrayIndex i = 0; i < input["aggregates"].size(); ++i) {
        const Json::Value &aggregate = input["aggregates"][i];
        if (!aggregate.isArray() || aggregate.size() != 3 ||
            !aggregate[0].isString() || !aggregate[1].isString() ||
            !aggregate[2].isString())
          throw std::invalid_argument("Malformed group-by aggregate!");
        const std::string function = aggregate[0].asString();
        double width = 4;
        const std::string argument = aggregate[1].asString();
        if (function == "avg")
          width = 18;
        else if (function == "min" || function == "max") {
          auto found = std::find_if(
              child.columns.begin(), child.columns.end(),
              [&](const auto &column) {
                return column.qualifier + "." + column.column_name == argument;
              });
          if (found == child.columns.end())
            throw std::invalid_argument(
                "MIN/MAX argument has no child column!");
          width = found->width;
        }
        result.relation.width += width;
        result.columns.push_back({"", aggregate[2].asString(),
                                  result.columns.size(), groups, width, nullptr});
      }
      result.relation.num_pages = estimator.output_pages(
          result.relation.num_tuples, result.relation.width);
    } else if (op == "sort" || op == "distinct" || op == "limit") {
      if (!input["child"].isObject())
        throw std::invalid_argument("Unary plan node requires a child!");
      result = visit(input["child"]);
      node["child"] = result.json;
      if (op == "limit") {
        const Json::Value &count = input["count"];
        if (!count.isUInt64() && !count.isInt())
          throw std::invalid_argument("Limit count must be an integer!");
        if (count.asInt64() < 0)
          throw std::invalid_argument("Limit count cannot be negative!");
        result.relation.num_tuples = std::min(
            result.relation.num_tuples,
            static_cast<std::size_t>(count.asUInt64()));
      } else if (op == "distinct") {
        std::size_t combinations = 1;
        for (const auto &column : result.columns) {
          const std::size_t ndv = estimator.distinct_values(
              column.relation_rows,
              column.statistics ? std::optional(column.statistics->ndv)
                                : std::nullopt);
          if (ndv != 0 && combinations >
                              result.relation.num_tuples / ndv) {
            combinations = result.relation.num_tuples;
            break;
          }
          combinations *= ndv;
        }
        result.relation.num_tuples =
            std::min(result.relation.num_tuples, combinations);
      }
      result.relation.num_pages = estimator.output_pages(
          result.relation.num_tuples, result.relation.width);
    } else {
      throw std::invalid_argument("Estimator does not support plan operator '" +
                                  op + "'!");
    }

    add_estimate_fields(node, result.relation);
    result.json = std::move(node);
    return result;
  };

  if (!std::isfinite(fallbacks_.unknown_range_selectivity) ||
      fallbacks_.unknown_range_selectivity < 0 ||
      fallbacks_.unknown_range_selectivity > 1 ||
      !std::isfinite(fallbacks_.varchar_bucket_fraction) ||
      fallbacks_.varchar_bucket_fraction < 0 ||
      fallbacks_.varchar_bucket_fraction > 1 ||
      !std::isfinite(fallbacks_.set_overlap_fraction) ||
      fallbacks_.set_overlap_fraction < 0 || fallbacks_.set_overlap_fraction > 1)
    throw std::invalid_argument("Invalid estimator fallback values!");
  return visit(logical_plan).json;
}

} // namespace vql::query
