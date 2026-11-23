#include "repl.h"

#include "lexer.h"

#include <iostream>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <variant>

void print_relation(const Relation &relation, std::ostream &output) {
  output << relation.name() << '\n';

  for (const Column &column : relation.schema().columns()) {
    output << column.name << '\t';
  }
  output << '\n';

  for (const Tuple &tuple : relation.tuples()) {
    for (const Value &value : tuple) {
      std::visit([&output](const auto &item) { output << item << '\t'; },
                 value);
    }
    output << '\n';
  }
}

static std::string attribute_name(const AttributeReference &attribute) {
  if (attribute.relation_name.has_value()) {
    return attribute.relation_name->name + "." + attribute.attribute_name.name;
  }
  return attribute.attribute_name.name;
}

static std::string operand_text(const Operand &operand) {
  if (const auto *number = std::get_if<double>(&operand.value)) {
    std::ostringstream output;
    output << *number;
    return output.str();
  }
  if (const auto *string = std::get_if<std::string>(&operand.value)) {
    return "'" + *string + "'";
  }
  return attribute_name(std::get<AttributeReference>(operand.value));
}

static std::string comparison_text(ComparisonOperator operation) {
  switch (operation) {
  case ComparisonOperator::Equal:
    return "=";
  case ComparisonOperator::NotEqual:
    return "!=";
  case ComparisonOperator::Less:
    return "<";
  case ComparisonOperator::LessEqual:
    return "<=";
  case ComparisonOperator::Greater:
    return ">";
  case ComparisonOperator::GreaterEqual:
    return ">=";
  }
  throw std::logic_error("UNKNOWN COMPARISON OPERATOR!!!!");
}

static std::string condition_text(const Condition &condition) {
  if (const auto *comparison =
          std::get_if<ComparisonCondition>(&condition.node)) {
    return operand_text(comparison->left) + " " +
           comparison_text(comparison->lil_durk) + " " +
           operand_text(comparison->right);
  }
  if (const auto *logical =
          std::get_if<LogicalCondition>(&condition.node)) {
    const std::string operation =
        logical->king_von == LogicalOperator::And ? " && " : " || ";
    return "(" + condition_text(*logical->left) + operation +
           condition_text(*logical->right) + ")";
  }
  const auto &negation = std::get<NotCondition>(condition.node);
  return "!(" + condition_text(*negation.operand) + ")";
}

static std::string binary_name(BinaryOperator operation) {
  switch (operation) {
  case BinaryOperator::UNION:
    return "Union";
  case BinaryOperator::INTERSECT:
    return "Intersect";
  case BinaryOperator::MINUS:
    return "Minus";
  case BinaryOperator::TIMES:
    return "Times";
  }
  throw std::logic_error("UNKNOWN BINARY OPERATOR!!!!");
}

static std::string
attributes_text(const std::vector<AttributeReference> &attributes) {
  std::string result;
  for (std::size_t i = 0; i < attributes.size(); ++i) {
    if (i > 0) {
      result += ", ";
    }
    result += attribute_name(attributes[i]);
  }
  return result;
}

void print_tree(const REX &expression, std::ostream &output, int depth) {
  output << std::string(depth * 2, ' ');

  if (const auto *relation = std::get_if<NameWithSpan>(&expression.node)) {
    output << "Relation(" << relation->name << ")\n";
    return;
  }
  if (const auto *binary =
          std::get_if<BinaryExpression>(&expression.node)) {
    output << binary_name(binary->operation) << '\n';
    print_tree(*binary->left, output, depth + 1);
    print_tree(*binary->right, output, depth + 1);
    return;
  }
  if (const auto *join = std::get_if<JoinExpression>(&expression.node)) {
    output << "Join(" << condition_text(join->condition) << ")\n";
    print_tree(*join->left, output, depth + 1);
    print_tree(*join->right, output, depth + 1);
    return;
  }
  if (const auto *selection =
          std::get_if<SelectExpression>(&expression.node)) {
    output << "Select(" << condition_text(selection->condition) << ")\n";
    print_tree(*selection->input, output, depth + 1);
    return;
  }
  if (const auto *projection =
          std::get_if<ProjectExpression>(&expression.node)) {
    output << "Project(" << attributes_text(projection->attributes) << ")\n";
    print_tree(*projection->input, output, depth + 1);
    return;
  }
  if (const auto *rename =
          std::get_if<RenameTableExpression>(&expression.node)) {
    output << "RenameTable(" << rename->new_name.name << ")\n";
    print_tree(*rename->input, output, depth + 1);
    return;
  }
  if (const auto *rename =
          std::get_if<RenameAttributeExpression>(&expression.node)) {
    output << "RenameAttribute(" << rename->new_name.name << ", "
           << attribute_name(rename->old_name) << ")\n";
    print_tree(*rename->input, output, depth + 1);
    return;
  }
  throw std::logic_error("UNKNOWN EXPRESSION!!!!");
}

std::string source_line(const std::string &source, std::size_t wanted_row) {
  std::size_t line_start = 0;

  for (std::size_t row = 1; row < wanted_row; ++row) {
    const std::size_t newline = source.find('\n', line_start);
    if (newline == std::string::npos) {
      return "";
    }
    line_start = newline + 1;
  }

  const std::size_t line_end = source.find('\n', line_start);
  if (line_end == std::string::npos) {
    return source.substr(line_start);
  }
  return source.substr(line_start, line_end - line_start);
}

void print_diagnostic(const std::string &category, const std::string &message,
                      const Span &span, const std::string &source) {
  std::cerr << category << " at " << span.begin.row << ':' << span.begin.col
            << ": " << message << '\n'
            << source_line(source, span.begin.row) << '\n'
            << std::string(span.begin.col - 1, ' ') << "^\n";
}

void print_parse_error(const ParseError &error, const std::string &source) {
  print_diagnostic("syntax error", error.message, error.span, source);
}

ParsedInput parse_repl_source(const std::string &source) {
  Result tokenized = Lexer::tokenize(source);

  if (tokenized.error.has_value()) {
    if (tokenized.error->type == EType::UnterminatedComment) {
      return {.status = ParseStatus::Incomplete,
              .statement = {},
              .incomplete_error = std::nullopt};
    }

    print_diagnostic("lexical error", tokenized.error->message,
                     tokenized.error->span, source);
    return {.status = ParseStatus::Error,
            .statement = {},
            .incomplete_error = std::nullopt};
  }

  auto parsed = Parser::parse_tokens(tokenized.tokens);

  if (const auto *error = std::get_if<ParseError>(&parsed)) {
    if (error->actual == TType::EndOfInput) {
      return {.status = ParseStatus::Incomplete,
              .statement = {},
              .incomplete_error = *error};
    }

    print_parse_error(*error, source);
    return {.status = ParseStatus::Error,
            .statement = {},
            .incomplete_error = std::nullopt};
  }

  return {.status = ParseStatus::Success,
          .statement = std::get<Statement>(std::move(parsed)),
          .incomplete_error = std::nullopt};
}
