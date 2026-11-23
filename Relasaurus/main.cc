#include "diagnostic.h"
#include "interpreter.h"
#include "repl.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <variant>

void print_stats(const Interpreter &interpreter, double wall_time,
                 std::size_t output_tuples) {
  const OperationStats &stats = interpreter.stats();
  std::cout << "SELECTION EXAMINATIONS: " << stats.selection_examinations
            << '\n'
            << "JOIN COMPARISONS: " << stats.join_comparisons << '\n'
            << "WALL TIME (S): " << wall_time << '\n'
            << "OUTPUT TUPLES: " << output_tuples << '\n';
}

void execute_statement(Interpreter &interpreter, const Statement &statement,
                       bool stats_mode) {
  try {
    const auto start = std::chrono::steady_clock::now();
    auto result = interpreter.execute(statement);
    const auto end = std::chrono::steady_clock::now();
    const double wall_time =
        std::chrono::duration<double>(end - start).count();

    if (result.has_value()) {
      print_relation(*result, std::cout);
      if (stats_mode) {
        print_stats(interpreter, wall_time, result->tuples().size());
      }
    } else {
      std::cout << "OK\n";
    }
  } catch (const DiagnosticError &error) {
    std::cerr << diagnostic_category_name(error.category())
              << " error: " << error.what() << '\n';
  } catch (const std::exception &error) {
    std::cerr << "runtime error: " << error.what() << '\n';
  }
}

void print_statement_tree(const Statement &statement) {
  const auto *query = std::get_if<REX>(&statement.guy);
  if (query == nullptr) {
    std::cerr << "TREE ERROR: EXPECTED A QUERY STATEMENT!!!!\n";
    return;
  }
  print_tree(*query, std::cout);
}

int main() {
  std::cout << "Choose a mode:\n"
            << "1) live\n"
            << "2) tree\n"
            << "3) stats\n"
            << "> " << std::flush;

  std::string choice;
  if (!std::getline(std::cin, choice)) {
    return 0;
  }

  if (choice != "1" && choice != "2" && choice != "3") {
    std::cerr << "INVALID MODE!!!!\n";
    return 1;
  }

  const bool tree_mode = choice == "2";
  const bool stats_mode = choice == "3";
  Interpreter interpreter;
  std::string source;
  std::string line;
  std::optional<ParseError> incomplete_error;

  while (true) {
    std::cout << (source.empty() ? "> " : "... ") << std::flush;

    if (!std::getline(std::cin, line)) {
      if (incomplete_error.has_value()) {
        print_parse_error(*incomplete_error, source);
      } else if (!source.empty()) {
        std::cerr << "SYNTAX ERROR: INCOMPLETE INPUT AT END OF FILE!!!!\n";
      }
      break;
    }

    if (source.empty() && (line == ":quit" || line == ":q")) {
      break;
    }

    if (source.empty() && line.empty()) {
      continue;
    }

    source += line + '\n';
    ParsedInput parsed = parse_repl_source(source);

    if (parsed.status == ParseStatus::Incomplete) {
      incomplete_error = parsed.incomplete_error;
      continue;
    }

    incomplete_error.reset();

    if (parsed.status == ParseStatus::Success) {
      if (tree_mode) {
        print_statement_tree(parsed.statement);
      } else {
        execute_statement(interpreter, parsed.statement, stats_mode);
      }
    }

    source.clear();
  }

  return 0;
}
