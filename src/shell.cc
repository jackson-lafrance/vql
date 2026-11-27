#include "shell.h"

#include "cost_model.h"
#include "diagnostic.h"
#include "estimator.h"
#include "executor.h"
#include "optimizer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "switcher.h"
#include "translator.h"

#include <bit>
#include <cctype>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace vql {

static std::string trim(std::string text) {
  const auto first = text.find_first_not_of(" \t\r\n");

  if (first == std::string::npos)
    return {};

  return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

static std::string upper(std::string text) {
  for (char &c : text)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

  return text;
}

static bool take_word(std::string &text, const std::string &word) {
  if (text.size() < word.size() || upper(text.substr(0, word.size())) != word ||
      (text.size() > word.size() &&
       !std::isspace(static_cast<unsigned char>(text[word.size()]))))
    return false;

  text = trim(text.substr(word.size()));
  return true;
}

static std::size_t number(const std::string &text, std::size_t minimum) {
  std::size_t value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);

  if (error != std::errc{} || end != text.data() + text.size() ||
      value < minimum)
    throw std::invalid_argument("Expected an integer of at least " +
                                std::to_string(minimum));

  return value;
}

static std::string identifier(const std::string &text) {
  if (text.empty() ||
      !(std::isalpha(static_cast<unsigned char>(text[0])) || text[0] == '_'))
    throw std::invalid_argument("Expected a table name");

  for (char c : text)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
      throw std::invalid_argument("Expected a table name, got: " + text);

  return text;
}

static std::int32_t integer(const std::string &text) {
  std::int32_t value = 0;
  const char *start = text.data();

  if (!text.empty() && text[0] == '+')
    ++start;

  const auto [end, error] =
      std::from_chars(start, text.data() + text.size(), value);

  if (error != std::errc{} || end != text.data() + text.size())
    throw std::invalid_argument("invalid INT: " + text);

  return value;
}

static std::vector<char> encode_fields(const algebra::Schema &schema,
                                       const std::vector<std::string> &fields) {
  if (fields.size() != schema.size())
    throw std::invalid_argument("Expected " + std::to_string(schema.size()) +
                                " fields, got " +
                                std::to_string(fields.size()));

  std::vector<char> record;

  for (std::size_t i = 0; i < schema.size(); ++i) {
    try {
      if (schema[i].type.data_type() == algebra::DataType::INT) {
        const auto bits = std::bit_cast<std::uint32_t>(integer(fields[i]));

        for (unsigned shift = 0; shift < 32; shift += 8)
          record.push_back(static_cast<char>((bits >> shift) & 255));
      } else {
        const auto &field = fields[i];

        if (field.size() > schema[i].type.max_length() ||
            field.size() > std::numeric_limits<std::uint16_t>::max())
          throw std::invalid_argument("VARCHAR exceeds declared length");

        record.push_back(static_cast<char>(field.size() & 255));
        record.push_back(static_cast<char>((field.size() >> 8) & 255));
        record.insert(record.end(), field.begin(), field.end());
      }
    } catch (const std::exception &error) {
      throw std::invalid_argument("column " + schema[i].name + ": " +
                                  error.what());
    }
  }

  algebra::switcher::validate_record(record, schema);
  return record;
}

static std::vector<std::string> csv_fields(const std::string &line) {
  std::vector<std::string> fields;
  std::size_t i = 0;

  for (;;) {
    std::string field;

    if (i < line.size() && line[i] == '"') {
      ++i;
      bool closed = false;

      while (i < line.size()) {
        if (line[i] != '"') {
          field += line[i++];
        } else if (i + 1 < line.size() && line[i + 1] == '"') {
          field += '"';
          i += 2;
        } else {
          ++i;
          closed = true;
          break;
        }
      }

      if (!closed || (i < line.size() && line[i] != ','))
        throw std::invalid_argument("Malformed quoted CSV field " +
                                    std::to_string(fields.size() + 1));
    } else {
      while (i < line.size() && line[i] != ',') {
        if (line[i] == '"')
          throw std::invalid_argument("Unexpected quote in CSV field " +
                                      std::to_string(fields.size() + 1));
        field += line[i++];
      }
    }

    fields.push_back(std::move(field));

    if (i == line.size())
      return fields;

    ++i;
  }
}

static std::string file_path(std::string text) {
  text = trim(text);

  if (text.empty())
    throw std::invalid_argument("Expected a file path after FROM");

  if (text.front() != '\'' && text.front() != '"') {
    if (text.find_first_of(" \t\r\n") != std::string::npos)
      throw std::invalid_argument("Quote file paths that contain spaces");
    return text;
  }

  const char quote = text.front();
  std::string result;

  for (std::size_t i = 1; i < text.size(); ++i) {
    if (text[i] != quote) {
      result += text[i];
    } else if (i + 1 < text.size() && text[i + 1] == quote) {
      result += quote;
      ++i;
    } else {
      if (i + 1 != text.size())
        throw std::invalid_argument("Unexpected text after file path");
      return result;
    }
  }

  throw std::invalid_argument("Unterminated file path");
}

static void print_value(std::ostream &out, const algebra::Value &value) {
  if (const auto *n = std::get_if<std::int32_t>(&value)) {
    out << *n;
  } else {
    // Quoted strings keep tabs and newlines from changing the result layout.
    Json::Value text(std::string(std::get<std::string_view>(value)));
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    out << Json::writeString(writer, text);
  }
}

static Json::Value logical_meaning(Json::Value node) {
  const std::string op = node["op"].asString();

  if (op == "seqscan")
    node["op"] = "scan";
  else if (op == "filter")
    node["op"] = "select";
  else if (op == "nested_loop_join" || op == "hash_join" ||
           op == "sort_merge_join")
    node["op"] = "join";

  for (const char *key : {"child", "left", "right"})
    if (node[key].isObject())
      node[key] = logical_meaning(node[key]);

  return node;
}

struct PlanLine {
  std::size_t depth;
  std::string label;
  std::size_t rows;
  std::size_t cost;
};

static std::size_t describe_plan(const Json::Value &physical,
                                 const Json::Value &estimate,
                                 const query::CostModel &costs,
                                 std::size_t depth,
                                 std::vector<PlanLine> &lines) {
  const auto slot = lines.size();
  std::string label = physical["op"].asString();

  if (physical["table"].isString())
    label += " " + physical["table"].asString();
  if (physical["mode"].isString())
    label += " (" + physical["mode"].asString() + ")";

  lines.push_back({depth, label, estimate["est_rows"].asUInt64(), 0});

  std::size_t cost = 0;

  for (const char *key : {"child", "left", "right"})
    if (physical[key].isObject())
      describe_plan(physical[key], estimate[key], costs, depth + 1, lines);

  const std::string op = physical["op"].asString();
  const auto pages = [&](const char *key) {
    return estimate[key]["est_pages"].asUInt64();
  };

  if (op == "seqscan")
    cost += costs.seqscan(estimate["est_pages"].asUInt64());
  else if (op == "sort")
    cost += costs.sort(pages("child"));
  else if (op == "nested_loop_join")
    cost += costs.nested_loop_join(pages("left"), pages("right"));
  else if (op == "hash_join")
    cost += costs.hash_join(pages("left"), pages("right"));
  else if (op == "sort_merge_join")
    cost += costs.sort_merge_join(pages("left"), pages("right"));
  else if (op == "distinct" || op == "group_by")
    cost += physical["mode"] == "hash" ? costs.hash_join(pages("child"), 0)
                                       : costs.sort(pages("child"));
  else if (op == "union" || op == "intersect" || op == "except")
    cost += costs.sort(pages("left") + pages("right"));

  lines[slot].cost = cost;
  return cost;
}

static void print_plan(std::ostream &out, const std::vector<PlanLine> &lines,
                       const query::ExecutionTrace *trace = nullptr) {
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const auto &line = lines[i];

    out << std::string(line.depth * 2, ' ') << line.label
        << " est_rows=" << line.rows << " est_cost=" << line.cost;

    if (trace && i < trace->nodes.size()) {
      const auto &counter = trace->nodes[i].counters;
      out << " actual_rows=" << counter.tuples_out
          << " reads=" << counter.page_reads
          << " writes=" << counter.page_writes
          << " comparisons=" << counter.comparisons;
    }
    out << '\n';
  }
}

Shell::Shell(std::filesystem::path directory, std::size_t frames)
    : directory_(std::move(directory)) {
  resize(frames);
}

void Shell::resize(std::size_t frames) {
  if (frames < 4)
    throw std::invalid_argument("frames must be at least 4");
  if (catalog_)
    catalog_->flush();
  if (pool_)
    pool_->flush_all();

  auto pool =
      std::make_unique<storage::BufferPool>(frames, storage::Policy::LRU);
  auto catalog = std::make_unique<storage::Catalog>(*pool, directory_);

  catalog_.reset();
  pool_.reset();
  pool_ = std::move(pool);
  catalog_ = std::move(catalog);
}

void Shell::execute(const std::string &command, std::ostream &out) {
  std::string rest = command;

  if (take_word(rest, "HELP")) {
    if (!rest.empty())
      throw std::invalid_argument("Usage: HELP;");
    out << "SQL: CREATE TABLE, INSERT INTO, SELECT, UNION, INTERSECT, EXCEPT\n"
           "EXPLAIN query;          show plan without execution\n"
           "EXPLAIN ANALYZE query;  execute and compare estimates with actual "
           "rows\n"
           "ANALYZE table;         collect persistent statistics (20 buckets)\n"
           "SET frames = n;        set buffer frames (minimum 4)\n"
           "LOAD table FROM 'file.csv';  load CSV without a header\n"
           "HELP;  QUIT;  EXIT;  or Ctrl-D to leave\n"
           "Commands end with ; and can span multiple lines.\n";
    return;
  }

  if (take_word(rest, "SET")) {
    const auto equal = rest.find('=');
    if (equal == std::string::npos ||
        upper(trim(rest.substr(0, equal))) != "FRAMES")
      throw std::invalid_argument("Usage: SET frames = n;");

    resize(number(trim(rest.substr(equal + 1)), 4));
    out << "frames=" << pool_->frames() << '\n';
    return;
  }

  query::StatisticsCollector statistics(*pool_, *catalog_);

  if (take_word(rest, "ANALYZE")) {
    const auto table = identifier(rest);
    statistics.analyze(table);
    catalog_->flush();
    out << "Analyzed " << table << '\n';
    return;
  }

  if (take_word(rest, "LOAD")) {
    const auto split = rest.find_first_of(" \t\r\n");
    if (split == std::string::npos)
      throw std::invalid_argument("Usage: LOAD table FROM 'file.csv';");
    const auto table = identifier(rest.substr(0, split));
    rest = trim(rest.substr(split));

    if (!take_word(rest, "FROM"))
      throw std::invalid_argument("Usage: LOAD table FROM 'file.csv';");
    const auto path = file_path(rest);
    const auto schema = catalog_->get_table(table).schema;
    std::ifstream file(path);
    if (!file)
      throw std::runtime_error("Cannot open CSV file: " + path);

    std::string line;
    std::size_t count = 0;

    while (std::getline(file, line)) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();

      try {
        const auto record = encode_fields(schema, csv_fields(line));
        catalog_->insert(table, record);
      } catch (const std::exception &error) {
        throw std::runtime_error(
            path + ": line " + std::to_string(count + 1) + ": " + error.what() +
            "; " + std::to_string(count) + " earlier rows remain loaded");
      }

      ++count;
    }

    if (file.bad())
      throw std::runtime_error("Read failed: " + path);
    catalog_->flush();
    out << "Loaded " << count << " rows into " << table << '\n';
    return;
  }

  const bool explain = take_word(rest, "EXPLAIN");
  const bool analyze = explain && take_word(rest, "ANALYZE");
  auto script = query::Parser::parse(rest + ";");
  if (script.statements.size() != 1)
    throw std::invalid_argument("Expected one SQL statement");

  auto &statement = script.statements.front();
  if (explain && !std::holds_alternative<query::Query>(statement))
    throw std::invalid_argument("EXPLAIN requires a query");

  query::SemanticAnalyzer(*catalog_).analyze(statement);

  if (const auto *create = std::get_if<query::CreateTable>(&statement)) {
    catalog_->create_table(create->table.name, *create->resolved_schema);
    catalog_->flush();
    out << "Created " << create->table.name << '\n';
  } else if (const auto *insert = std::get_if<query::Insert>(&statement)) {
    std::vector<std::string> fields;

    for (const auto &literal : insert->values) {
      if (const auto *n = std::get_if<std::int32_t>(&literal.value))
        fields.push_back(std::to_string(*n));
      else
        fields.push_back(std::get<std::string>(literal.value));
    }

    const auto schema = catalog_->get_table(insert->table.name).schema;
    const auto record = encode_fields(schema, fields);
    catalog_->insert(insert->table.name, record);
    catalog_->flush();
    out << "Inserted 1 row\n";
  } else {
    const auto logical =
        query::Translator().translate(std::get<query::Query>(statement));
    const auto plan = query::Optimizer(*catalog_, statistics, pool_->frames())
                          .optimize(logical);
    const auto estimated = query::JsonPlanEstimator(*catalog_, statistics)
                               .estimate(logical_meaning(plan));

    std::vector<PlanLine> lines;
    describe_plan(plan, estimated, query::CostModel(pool_->frames()), 0, lines);

    if (explain && !analyze) {
      print_plan(out, lines);
      Json::StreamWriterBuilder writer;
      writer["indentation"] = "  ";
      out << "Physical plan:\n" << Json::writeString(writer, plan) << '\n';
      return;
    }

    const auto before = pool_->stats();
    const auto start = std::chrono::steady_clock::now();
    const auto trace =
        query::Executor(*pool_, *catalog_)
            .run(
                plan,
                [&](const algebra::Schema &schema) {
                  if (analyze)
                    return;
                  for (std::size_t i = 0; i < schema.size(); ++i) {
                    if (i)
                      out << '\t';
                    out << schema[i].name;
                  }
                  out << '\n';
                },
                [&](const algebra::Tuple &row) {
                  if (analyze)
                    return;
                  for (std::size_t i = 0; i < row.size(); ++i) {
                    if (i)
                      out << '\t';
                    print_value(out, row[i]);
                  }
                  out << '\n';
                });

    const double elapsed = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - start)
                               .count();
    const auto after = pool_->stats();
    if (analyze)
      print_plan(out, lines, &trace);

    out << "rows=" << trace.totals.tuples_out
        << " page_reads=" << after.reads - before.reads
        << " page_writes=" << after.writes - before.writes
        << " elapsed_ms=" << std::fixed << std::setprecision(3) << elapsed
        << " est_rows=" << lines.front().rows
        << " actual_rows=" << trace.totals.tuples_out << '\n';
  }
}

int Shell::run(std::istream &input, std::ostream &out, std::ostream &errors,
               bool prompt) {
  std::string pending;
  char quote = 0;
  bool failed = false;
  bool stop = false;

  if (prompt)
    out << "VQL. End commands with ;. Type HELP; for commands.\n";

  std::string line;

  while (!stop) {
    if (prompt)
      out << (pending.empty() ? "vql> " : " ...> ") << std::flush;
    if (!std::getline(input, line))
      break;

    if (pending.empty() &&
        (upper(trim(line)) == "QUIT" || upper(trim(line)) == "EXIT"))
      break;

    for (std::size_t i = 0; i < line.size(); ++i) {
      const char c = line[i];

      if (quote) {
        pending += c;
        if (c == quote) {
          if (i + 1 < line.size() && line[i + 1] == quote)
            pending += line[++i];
          else
            quote = 0;
        }
      } else if (c == '-' && i + 1 < line.size() && line[i + 1] == '-') {
        break;
      } else if (c == '\'' || c == '"') {
        quote = c;
        pending += c;
      } else if (c == ';') {
        const auto command = trim(pending);
        pending.clear();

        if (command.empty())
          continue;

        if (upper(command) == "QUIT" || upper(command) == "EXIT") {
          stop = true;
          break;
        }

        try {
          execute(command, out);
        } catch (const query::DiagnosticError &error) {
          failed = true;
          errors << query::error_category_name(error.category()) << " at "
                 << error.span().begin.row << ':' << error.span().begin.col
                 << ": " << error.what() << '\n';
        } catch (const std::exception &error) {
          failed = true;
          errors << "Error: " << error.what() << '\n';
        }
      } else {
        pending += c;
      }
    }

    if (!pending.empty())
      pending += '\n';
    if (!quote && trim(pending).empty())
      pending.clear();
  }

  if (!stop && !trim(pending).empty()) {
    errors << "Error: incomplete command at end of input (expected "
           << (quote ? "closing quote and ;" : ";") << ")\n";
    failed = true;
  }

  catalog_->flush();
  pool_->flush_all();

  return !prompt && failed ? 1 : 0;
}

} // namespace vql
