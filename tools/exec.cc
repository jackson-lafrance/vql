#include "common.h"
#include "executor.h"

#include <iostream>

namespace vql::cli::exec_tool {

Json::Value counters(const algebra::Operator::Counter &count) {
  Json::Value result(Json::objectValue);
  result["tuples_out"] = Json::UInt64(count.tuples_out);
  result["comparisons"] = Json::UInt64(count.comparisons);
  result["page_reads"] = Json::UInt64(count.page_reads);
  result["page_writes"] = Json::UInt64(count.page_writes);
  return result;
}

int run(int argc, char **argv) {
  return cli::guarded([&] {
    cli::Args args(argc, argv);

    if (args.command == "--help" || args.has("--help")) {
      std::cout << "exec run --db DIR --plan plan.json --frames 32 "
                   "--json\nexec trace --db DIR --plan plan.json --frames 32\n";
      return;
    }

    if (args.command != "run" && args.command != "trace")
      throw std::invalid_argument("Unknown exec command " + args.command);
    args.allow({"--db", "--plan", "--frames", "--json"});

    const auto plan = cli::read_json(args.require("--plan"));
    cli::Database db(args);
    query::Executor executor(db.pool, db.catalog);

    bool first = true;
    const bool run = args.command == "run";
    const bool json = args.has("--json");

    const auto trace = executor.run(
        plan,
        [&](const algebra::Schema &schema) {
          if (run && json)
            std::cout << "{\"schema\":" << cli::json(cli::schema_json(schema))
                      << ",\"rows\":[";
        },
        [&](const algebra::Tuple &tuple) {
          if (!run)
            return;

          if (json && !first)
            std::cout << ',';
          std::cout << cli::json(cli::row_json(tuple));
          if (!json)
            std::cout << '\n';
          first = false;
        });

    if (run && json)
      std::cout << "],\"totals\":" << cli::json(counters(trace.totals))
                << "}\n";

    if (!run) {
      for (const auto &node : trace.nodes) {
        const auto &c = node.counters;
        std::cout << std::string(node.depth * 2, ' ') << node.label
                  << " tuples_out=" << c.tuples_out
                  << " comparisons=" << c.comparisons
                  << " reads=" << c.page_reads << " writes=" << c.page_writes
                  << '\n';
      }
    }
  });
}

} // namespace vql::cli::exec_tool

int main(int argc, char **argv) { return vql::cli::exec_tool::run(argc, argv); }
