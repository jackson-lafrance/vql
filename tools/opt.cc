#include "common.h"
#include "optimizer.h"
#include "statistics_collector.h"

#include <iostream>

namespace vql::cli::opt_tool {

Json::Value logical_form(Json::Value plan) {
  const auto op = plan["op"].asString();
  if (op == "seqscan")
    plan["op"] = "scan";
  else if (op == "filter")
    plan["op"] = "select";
  else if (op == "nested_loop_join" || op == "hash_join" ||
           op == "sort_merge_join")
    plan["op"] = "join";
  for (const char *key : {"child", "left", "right"})
    if (plan.isMember(key))
      plan[key] = logical_form(plan[key]);
  return plan;
}

void explain(const Json::Value &physical, const Json::Value &estimate,
             const query::CostModel &costs, std::size_t depth = 0) {
  const auto op = physical["op"].asString();
  const auto pages = [&](const char *key) {
    return static_cast<std::size_t>(estimate[key]["est_pages"].asUInt64());
  };
  std::size_t cost = 0;
  if (op == "seqscan")
    cost = costs.seqscan(estimate["est_pages"].asUInt64());
  else if (op == "nested_loop_join")
    cost = costs.nested_loop_join(pages("left"), pages("right"));
  else if (op == "hash_join")
    cost = costs.hash_join(pages("left"), pages("right"));
  else if (op == "sort_merge_join")
    cost = costs.sort_merge_join(pages("left"), pages("right"));
  else if (op == "sort" || op == "distinct" || op == "group_by" ||
           op == "union" || op == "intersect" || op == "except") {
    const auto input_pages = physical.isMember("child")
                                 ? pages("child")
                                 : pages("left") + pages("right");
    cost = physical.get("mode", "sort").asString() == "hash"
               ? costs.hash_join(input_pages, 0)
               : costs.sort(input_pages);
  }
  std::cout << std::string(depth * 2, ' ') << op;
  if (physical.isMember("table"))
    std::cout << ' ' << physical["table"].asString();
  if (physical.isMember("condition"))
    std::cout << " (" << cli::json(physical["condition"]) << ')';
  std::cout << " est_rows=" << estimate["est_rows"].asUInt64()
            << " est_cost=" << cost << '\n';
  for (const char *key : {"child", "left", "right"})
    if (physical.isMember(key))
      explain(physical[key], estimate[key], costs, depth + 1);
}

std::string plan_summary(const Json::Value &plan) {
  if (plan["op"] == "seqscan")
    return plan["table"].asString();
  std::string result = plan["op"].asString() + "(";
  bool first = true;
  for (const char *key : {"child", "left", "right"}) {
    if (!plan.isMember(key))
      continue;
    if (!first)
      result += ", ";
    result += plan_summary(plan[key]);
    first = false;
  }
  return result + ")";
}

int run(int argc, char **argv) {
  return cli::guarded([&] {
    cli::Args args(argc, argv);

    if (args.command == "--help" || args.has("--help")) {
      std::cout << "opt analyze --db DIR --table R [--buckets 20]\nopt plan "
                   "--db DIR --logical q.json --frames 32 --json\nopt explain "
                   "--db DIR --logical q.json --frames 32\nopt dp-table --db "
                   "DIR --logical q.json --frames 32\n";
      return;
    }

    if (args.command == "analyze")
      args.allow({"--db", "--catalog", "--table", "--buckets", "--frames"});
    else if (args.command == "plan" || args.command == "explain" ||
             args.command == "dp-table")
      args.allow({"--db", "--catalog", "--logical", "--frames", "--json"});
    else
      throw std::invalid_argument("Unknown opt command " + args.command);

    cli::Database db(args);
    query::StatisticsCollector statistics(db.pool, db.catalog);

    if (args.command == "analyze") {
      statistics.analyze(args.require("--table"), args.number("--buckets", 20));
      db.catalog.flush();
      return;
    }

    const auto logical = cli::read_json(args.require("--logical"));
    query::Optimizer optimizer(db.catalog, statistics, db.pool.frames());

    if (args.command == "dp-table") {
      const auto rows = optimizer.dp_table(logical);
      std::cout
          << "subset\tbest plan (left/outer first)\test_rows\tpages\tcost\n";
      for (const auto &row : rows) {
        std::cout << '{';
        for (Json::ArrayIndex i = 0; i < row["subset"].size(); ++i) {
          if (i)
            std::cout << ',';
          std::cout << row["subset"][i].asString();
        }
        std::cout << "}\t";
        if (!row.isMember("plan"))
          std::cout << "skipped\n";
        else
          std::cout << plan_summary(row["plan"]) << '\t'
                    << row["rows"].asUInt64() << '\t' << row["pages"].asUInt64()
                    << '\t' << row["cost"].asUInt64() << '\n';
      }
      return;
    }

    const auto physical = optimizer.optimize(logical);
    if (args.command == "plan")
      std::cout << cli::json(physical) << '\n';
    else {
      const auto estimate = query::JsonPlanEstimator(db.catalog, statistics)
                                .estimate(logical_form(physical));
      explain(physical, estimate, query::CostModel(db.pool.frames()));
    }
  });
}

} // namespace vql::cli::opt_tool

int main(int argc, char **argv) { return vql::cli::opt_tool::run(argc, argv); }
