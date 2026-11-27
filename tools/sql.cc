#include "common.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "translator.h"

#include <iostream>

namespace vql::cli::sql_tool {

class TreePrinter {
public:
  explicit TreePrinter(const std::string &source) : source_(source) {}

  void script(const query::Script &script) const {
    line(0, "Script");
    for (const auto &statement : script.statements) {
      if (const auto *q = std::get_if<query::Query>(&statement))
        query(*q, 1);
      else if (const auto *create =
                   std::get_if<query::CreateTable>(&statement)) {
        line(1, "CreateTable " + create->table.name);
        for (const auto &column : create->columns)
          line(2, "Column " + text(column.span));
      } else {
        const auto &insert = std::get<query::Insert>(statement);
        line(1, "Insert " + insert.table.name);
        for (const auto &value : insert.values)
          line(2, "Literal " + text(value.span));
      }
    }
  }

private:
  const std::string &source_;

  static void line(std::size_t depth, const std::string &label) {
    std::cout << std::string(depth * 2, ' ') << label << '\n';
  }

  std::string text(query::Span span) const {
    auto result =
        source_.substr(span.begin.offset, span.end.offset - span.begin.offset);
    for (char &c : result)
      if (c == '\n' || c == '\r' || c == '\t')
        c = ' ';
    return result;
  }

  void condition(const query::Condition &c, std::size_t depth) const {
    if (const auto *logical = std::get_if<query::LogicalCondition>(&c.value)) {
      line(depth, logical->op == query::LogicalOperator::AND ? "And" : "Or");
      condition(*logical->left, depth + 1);
      condition(*logical->right, depth + 1);
    } else if (const auto *negation =
                   std::get_if<query::NotCondition>(&c.value)) {
      line(depth, "Not");
      condition(*negation->child, depth + 1);
    } else
      line(depth, "Compare " + text(c.span));
  }

  void table(const query::TableRef &ref, std::size_t depth) const {
    if (const auto *named = std::get_if<query::NamedTable>(&ref.value))
      line(depth, "Table " + named->name.name +
                      (named->alias ? " AS " + named->alias->name : ""));
    else {
      const auto &sub = std::get<query::SubqueryTable>(ref.value);
      line(depth, "Subquery AS " + sub.alias.name);
      query(*sub.query, depth + 1);
    }
  }

  void query(const query::Query &q, std::size_t depth) const {
    if (const auto *set = std::get_if<query::SetOperation>(&q.value)) {
      const char *name = set->op == query::SetOperator::UNION       ? "Union"
                         : set->op == query::SetOperator::UNION_ALL ? "UnionAll"
                         : set->op == query::SetOperator::INTERSECT
                             ? "Intersect"
                             : "Except";
      line(depth, name);
      query(*set->left, depth + 1);
      query(*set->right, depth + 1);
      return;
    }
    const auto &select = std::get<query::SelectBlock>(q.value);
    line(depth, select.distinct ? "Select DISTINCT" : "Select");
    line(depth + 1, "Columns");
    if (std::holds_alternative<query::Star>(select.columns))
      line(depth + 2, "Star");
    else
      for (const auto &item :
           std::get<std::vector<query::SelectItem>>(select.columns))
        line(depth + 2, text(item.span));
    line(depth + 1, "From");
    if (const auto *comma = std::get_if<query::CommaFrom>(&select.from)) {
      for (const auto &ref : comma->tables)
        table(ref, depth + 2);
    } else {
      const auto &join = std::get<query::JoinFrom>(select.from);
      table(join.first, depth + 2);
      for (const auto &clause : join.joins) {
        line(depth + 2, "Join");
        table(clause.right, depth + 3);
        line(depth + 3, "On");
        condition(clause.on, depth + 4);
      }
    }
    if (select.where) {
      line(depth + 1, "Where");
      condition(*select.where, depth + 2);
    }
    if (select.group_by) {
      line(depth + 1, "GroupBy");
      for (const auto &column : select.group_by->columns)
        line(depth + 2, text(column.span));
      if (select.group_by->having) {
        line(depth + 1, "Having");
        condition(*select.group_by->having, depth + 2);
      }
    }
    if (!select.order_by.empty()) {
      line(depth + 1, "OrderBy");
      for (const auto &key : select.order_by)
        line(depth + 2,
             text(key.column.span) + (key.direction == query::SortDirection::ASC
                                          ? " ASC"
                                          : " DESC"));
    }
    if (select.limit)
      line(depth + 1, "Limit " + std::to_string(select.limit->count));
  }
};

int run(int argc, char **argv) {
  return cli::guarded([&] {
    cli::Args args(argc, argv);

    if (args.command == "--help" || args.has("--help")) {
      std::cout
          << "sql parse --query \"SELECT ...\" --explain\nsql compile --db DIR "
             "--file q.sql --json\nsql check --db DIR --file q.sql\n";
      return;
    }

    if (args.command == "parse")
      args.allow({"--query", "--file", "--explain"});
    else if (args.command == "compile" || args.command == "check")
      args.allow({"--db", "--catalog", "--file", "--json", "--frames"});
    else
      throw std::invalid_argument("Unknown sql command " + args.command);

    if (args.has("--query") && args.has("--file"))
      throw std::invalid_argument("Use --query or --file, not both");

    const auto source = args.has("--query")
                            ? args.require("--query")
                            : cli::read_text(args.require("--file"));
    auto script = query::Parser::parse(source);

    if (args.command == "parse") {
      TreePrinter(source).script(script);
      return;
    }

    cli::Database db(args);
    query::SemanticAnalyzer(db.catalog).analyze(script);

    if (args.command == "check")
      return;

    if (script.statements.size() != 1 ||
        !std::holds_alternative<query::Query>(script.statements[0]))
      throw std::invalid_argument("compile requires exactly one SELECT or "
                                  "set-operation query; use check for scripts");

    std::cout << cli::json(query::Translator().translate(
                     std::get<query::Query>(script.statements[0])))
              << '\n';
  });
}

} // namespace vql::cli::sql_tool

int main(int argc, char **argv) { return vql::cli::sql_tool::run(argc, argv); }
