#include "diagnostic.h"
#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include "repl.h"
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

using std::string;

struct Parsed {
  Result tokens;
  Statement statement;
};

int passed = 0;
int failed = 0;

void check(bool condition, const string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

template <class Function>
void test(int number, const string &name, Function run) {
  std::cout << "\n[TEST " << number << "] " << name << '\n';
  try {
    run();
    ++passed;
    std::cout << "PASS\n";
  } catch (const std::exception &error) {
    ++failed;
    std::cout << "FAIL: " << error.what() << '\n';
  }
}

string tree_text(const Statement &statement) {
  std::ostringstream output;
  print_tree(std::get<REX>(statement.guy), output);
  return output.str();
}

Parsed parse(const string &source, bool show = true) {
  Result tokens = Lexer::tokenize(source);
  if (show) {
    std::cout << "query: " << source << "\ntokens:";
    for (auto &token : tokens.tokens) {
      std::cout << ' ' << (token.type == TType::EndOfInput ? "<eof>" : token.lexeme);
      if (auto text = std::get_if<string>(&token.value)) std::cout << '=' << std::quoted(*text);
    }
    std::cout << '\n';
  }
  check(!tokens.error, "unexpected lexer error");
  auto result = Parser::parse_tokens(tokens.tokens);
  check(std::holds_alternative<Statement>(result), "unexpected syntax error");
  Statement statement = std::get<Statement>(std::move(result));
  if (show) {
    std::cout << "tree:\n";
    print_tree(std::get<REX>(statement.guy), std::cout);
  }
  return {std::move(tokens), std::move(statement)};
}

ParseError syntax_error(const string &source) {
  Result tokens = Lexer::tokenize(source);
  auto result = Parser::parse_tokens(tokens.tokens);
  check(std::holds_alternative<ParseError>(result), "expected syntax error");
  auto error = std::get<ParseError>(result);
  std::cout << "query: " << source << "\nsyntax error at " << error.span.begin.row << ':'
            << error.span.begin.col << ": " << error.message << '\n';
  return error;
}

Interpreter database(std::initializer_list<string> definitions) {
  Interpreter interpreter;
  for (auto &definition : definitions) {
    auto parsed = parse(definition, false);
    check(!interpreter.execute(parsed.statement), "definition returned a relation");
  }
  return interpreter;
}

Relation query(Interpreter &interpreter, const string &source) {
  auto parsed = parse(source);
  auto result = interpreter.execute(parsed.statement);
  check(result.has_value(), "query returned nothing");
  std::cout << "result:\n";
  print_relation(*result, std::cout);
  return std::move(*result);
}

string runtime_error(Interpreter &interpreter, const string &source) {
  auto parsed = parse(source);
  try {
    interpreter.execute(parsed.statement);
  } catch (const DiagnosticError &error) {
    std::cout << diagnostic_category_name(error.category())
              << " error: " << error.what() << '\n';
    return error.what();
  } catch (const std::runtime_error &error) {
    std::cout << "runtime error: " << error.what() << '\n';
    return error.what();
  }
  throw std::runtime_error("expected runtime error");
}

const Token &expect_token(const Parsed &parsed, size_t index, TType type) {
  check(index < parsed.tokens.tokens.size() && parsed.tokens.tokens[index].type == type, "wrong token");
  return parsed.tokens.tokens[index];
}

void expect_tree(const string &source, const string &expected) {
  auto parsed = parse(source);
  check(tree_text(parsed.statement) == expected, "wrong tree");
}

int main() {
  test(1, "compact select", [] {
    auto p = parse("select{x1=3}(R);");
    expect_token(p, 2, TType::Identifier);
    expect_token(p, 3, TType::Equal);
    expect_token(p, 4, TType::Number);
  });
  test(2, "whitespace is ignored", [] {
    auto spaced = parse("select{ x1 = 3 }(R);");
    auto compact = parse("select{x1=3}(R);", false);
    check(tree_text(spaced.statement) == tree_text(compact.statement),
          "trees differ");
  });
  test(3, ">= is one token", [] { expect_token(parse("select{Age>=30}(R);"), 3, TType::GreaterEqual); });
  test(4, "> and -30 are separate", [] {
    auto p = parse("select{Age>-30}(R);"); expect_token(p, 3, TType::Greater);
    check(std::get<double>(expect_token(p, 4, TType::Number).value) == -30, "wrong number");
  });
  test(5, "parenthesis inside string", [] {
    auto p = parse("select{Name='Bob)'}(R);");
    check(std::get<string>(expect_token(p, 4, TType::String).value) == "Bob)", "wrong string");
  });
  test(6, "comma inside string", [] {
    auto p = parse("select{Name='a,b'}(R);");
    check(std::get<string>(expect_token(p, 4, TType::String).value) == "a,b", "wrong string");
  });
  test(7, "doubled quote", [] {
    auto p = parse("select{Name='O''Brien'}(R);");
    check(std::get<string>(expect_token(p, 4, TType::String).value) == "O'Brien", "wrong string");
  });
  test(8, "union can be an attribute", [] { expect_token(parse("select{union=3}(R);"), 2, TType::Identifier); });
  test(9, "unterminated string", [] {
    string source = "select{Name='Bob}(R);";
    Result result = Lexer::tokenize(source);
    check(result.error && result.error->type == EType::UnterminatedString &&
              result.error->span.begin.offset == source.find('\''),
          "wrong error");
    std::cout << "lexical error at " << result.error->span.begin.row << ':'
              << result.error->span.begin.col << ": "
              << result.error->message << '\n';
  });
  test(10, "union/minus grouping", [] {
    expect_tree("A+B-C;",
                "Minus\n"
                "  Union\n"
                "    Relation(A)\n"
                "    Relation(B)\n"
                "  Relation(C)\n");
  });
  test(11, "minus is left associative", [] {
    auto i = database({"relation A(ID=number){1;};", "relation B(ID=number){1;};", "relation C(ID=number){1;};"});
    check(query(i, "A-B-C;").tuples().size() == 0, "wrong left grouping");
    check(query(i, "A-(B-C);").tuples().size() == 1, "groupings should differ");
  });
  test(12, "not/and/or precedence", [] {
    expect_tree("select{!(a=1&&b=2)||c>3}(R);",
                "Select((!((a = 1 && b = 2)) || c > 3))\n"
                "  Relation(R)\n");
  });
  test(13, "and binds before or", [] {
    expect_tree("select{a=1&&b=2||c=3}(R);",
                "Select(((a = 1 && b = 2) || c = 3))\n"
                "  Relation(R)\n");
  });
  test(14, "three nested operations", [] {
    auto i = database({"relation Employees(Name=string,Age=number,DID=string){Ann,35,D1;Bob,40,D2;Cara,25,D1;};"});
    auto r = query(i, "project{Name}(select{Age>30}(select{DID='D1'}(Employees)));");
    check(r.tuples().size() == 1 && r.contains(Tuple{string("Ann")}), "wrong result");
  });
  test(15, "parentheses override grouping", [] {
    expect_tree("(A+B)-(C&D);",
                "Minus\n"
                "  Union\n"
                "    Relation(A)\n"
                "    Relation(B)\n"
                "  Intersect\n"
                "    Relation(C)\n"
                "    Relation(D)\n");
  });
  test(16, "missing parenthesis", [] {
    auto error = syntax_error("select{Age>30}(R;");
    check(error.expected && *error.expected == TType::RightParen,
          "wrong error");
  });
  test(17, "empty projection", [] {
    auto error = syntax_error("project{}(R);");
    check(error.expected && *error.expected == TType::Identifier,
          "wrong error");
  });
  test(18, "compare two columns", [] {
    auto i = database({"relation R(A=number,B=number){1,1;2,3;};"}); auto r = query(i, "select{A=B}(R);");
    check(r.tuples().size() == 1 && r.contains(Tuple{1.0, 1.0}), "wrong result");
  });
  test(19, "qualified join", [] {
    auto i = database({
        "relation Emp(EID=number,DID=string){1,D1;2,D2;};",
        "relation Dept(DID=string,Name=string){D1,Sales;D3,Legal;};",
    });
    auto r = query(i, "Emp@{Emp.DID=Dept.DID}Dept;");
    check(r.tuples().size() == 1 &&
              r.schema().index_of("Emp.DID") != Schema::npos &&
              r.schema().index_of("Dept.DID") != Schema::npos,
          "wrong join");
  });
  test(20, "renamed self join", [] {
    auto i = database({"relation Emp(EID=number,MgrID=number){1,0;2,1;3,1;};"});
    check(query(i, "renameTable{E2}(Emp)@{Emp.MgrID=E2.EID}Emp;").tuples().size() == 2, "wrong self join");
  });
  test(21, "union schema error", [] {
    auto i = database({"relation R(ID=number){1;};", "relation S(Name=string){A;};"});
    check(runtime_error(i, "R+S;").find("SCHEMA") != string::npos, "unclear error");
  });
  test(22, "comparison type error", [] {
    auto i = database({"relation R(Age=number){20;};"});
    check(runtime_error(i, "select{Age>'30'}(R);").find("CANNOT COMPARE") != string::npos, "unclear error");
  });
  test(23, "projection removes duplicates", [] {
    auto i = database({"relation Employees(Name=string,DID=string){Ann,D1;Bob,D1;Cara,D2;};"});
    check(query(i, "project{DID}(Employees);").tuples().size() == 2, "duplicates remain");
  });
  test(24, "duplicate projected attributes collapse", [] {
    auto i = database({"relation R(Name=string){Ann;};"});
    check(query(i, "project{Name,Name}(R);").schema().columns().size() == 1, "duplicate column remains");
  });
  test(25, "empty result prints schema", [] {
    auto i = database({"relation R(Name=string,Age=number){Ann,20;};"}); auto r = query(i, "select{Age>100}(R);");
    std::ostringstream out; print_relation(r, out); check(out.str() == "R\nName\tAge\t\n", "bad empty output");
  });

  std::cout << "\n" << passed << " passed, " << failed << " failed\n";
  return failed ? 1 : 0;
}
