#include "operators/sort_groupby_operator.h"

#include "switcher.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace vql::algebra {

using OwnedValue = std::variant<std::int32_t, std::string>;

static int compare_owned(const OwnedValue &left, const OwnedValue &right) {
  if (left.index() != right.index())
    throw std::logic_error("GROUP BY compared values of different types!");
  if (std::holds_alternative<std::int32_t>(left)) {
    const auto left_int = std::get<std::int32_t>(left);
    const auto right_int = std::get<std::int32_t>(right);
    return left_int < right_int ? -1 : left_int > right_int ? 1 : 0;
  }
  const int result =
      std::get<std::string>(left).compare(std::get<std::string>(right));
  return result < 0 ? -1 : result > 0 ? 1 : 0;
}

static std::string
function_name(GroupbyOperator::Aggregate::Function function) {
  switch (function) {
  case GroupbyOperator::Aggregate::Function::COUNT:
    return "count";
  case GroupbyOperator::Aggregate::Function::SUM:
    return "sum";
  case GroupbyOperator::Aggregate::Function::AVG:
    return "avg";
  case GroupbyOperator::Aggregate::Function::MIN:
    return "min";
  case GroupbyOperator::Aggregate::Function::MAX:
    return "max";
  }
  throw std::invalid_argument("Invalid GROUP BY aggregate function!");
}

static void append_int(std::vector<char> &record, std::int32_t value) {
  const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
  for (unsigned shift = 0; shift < 32; shift += 8)
    record.push_back(static_cast<char>((bits >> shift) & 0xFF));
}

static void append_varchar(std::vector<char> &record, std::string_view value) {
  if (value.size() > std::numeric_limits<std::uint16_t>::max())
    throw std::length_error("GROUP BY output VARCHAR is too long!");
  const auto length = static_cast<std::uint16_t>(value.size());
  record.push_back(static_cast<char>(length & 0xFF));
  record.push_back(static_cast<char>((length >> 8) & 0xFF));
  record.insert(record.end(), value.begin(), value.end());
}

SortGroupbyOperator::SortGroupbyOperator(std::unique_ptr<Operator> input,
                                         storage::BufferPool &buffer_pool,
                                         std::size_t workspace_frames,
                                         std::vector<std::string> key_names,
                                         std::vector<Aggregate> aggregates)
    : buffer_pool_(buffer_pool), workspace_frames_(workspace_frames),
      key_names_(std::move(key_names)), aggregates_(std::move(aggregates)) {
  if (!input)
    throw std::invalid_argument("Sort GROUP BY is missing an input operator!");
  if (workspace_frames_ < 3)
    throw std::invalid_argument(
        "Sort GROUP BY requires at least three workspace frames!");

  const Schema &input_schema = input->schema();
  key_positions_.reserve(key_names_.size());
  std::vector<SortKey> sort_keys;
  sort_keys.reserve(key_names_.size());
  for (const std::string &name : key_names_) {
    const std::size_t position = input_schema.index_of(name);
    key_positions_.push_back(position);
    sort_keys.push_back({position, true});
  }

  aggregate_positions_.reserve(aggregates_.size());
  std::vector<Column> output_columns;
  output_columns.reserve(key_positions_.size() + aggregates_.size());
  for (std::size_t position : key_positions_)
    output_columns.push_back(input_schema[position]);

  for (const Aggregate &aggregate : aggregates_) {
    const std::string function = function_name(aggregate.function);
    if (aggregate.column == "*") {
      if (aggregate.function != Aggregate::Function::COUNT)
        throw std::invalid_argument("Only COUNT accepts '*'!");
      aggregate_positions_.push_back(input_schema.size());
    } else {
      if (aggregate.column.empty())
        throw std::invalid_argument("GROUP BY aggregate column is empty!");
      const std::size_t position = input_schema.index_of(aggregate.column);
      const DataType input_type = input_schema[position].type.data_type();
      if ((aggregate.function == Aggregate::Function::SUM ||
           aggregate.function == Aggregate::Function::AVG) &&
          input_type != DataType::INT)
        throw std::invalid_argument(function + " requires an INT column!");
      aggregate_positions_.push_back(position);
    }

    const std::string default_name =
        function + "_" + (aggregate.column == "*" ? "star" : aggregate.column);
    const std::string output_name =
        aggregate.output_name.empty() ? default_name : aggregate.output_name;
    Type output_type(DataType::INT);
    if (aggregate.function == Aggregate::Function::AVG)
      output_type = Type(DataType::VARCHAR, 16);
    else if (aggregate.function == Aggregate::Function::MIN ||
             aggregate.function == Aggregate::Function::MAX) {
      if (aggregate.column == "*")
        throw std::invalid_argument("MIN and MAX require a column!");
      output_type = input_schema[aggregate_positions_.back()].type;
    }
    output_columns.push_back({output_name, output_type});
  }

  for (std::size_t i = 0; i < output_columns.size(); ++i)
    for (std::size_t j = 0; j < i; ++j)
      if (output_columns[i].name == output_columns[j].name)
        throw std::invalid_argument(
            "GROUP BY output schema contains duplicate column names!");
  schema_ = Schema(std::move(output_columns));
  sorter_ = std::make_unique<SortOperator>(
      std::move(input), buffer_pool_, std::move(sort_keys), workspace_frames_);
}

const Schema &SortGroupbyOperator::schema() const { return schema_; }

void SortGroupbyOperator::open() {
  if (state_ != State::CREATED)
    throw std::logic_error("Sort GROUP BY can only be opened once!");

  state_ = State::FAILED;
  sorter_needs_close_ = true;
  sorter_->open();
  state_ = State::OPEN;
}

std::optional<Tuple> SortGroupbyOperator::next_input() {
  if (pending_record_.has_value())
    return switcher::decode(*pending_record_, sorter_->schema());
  auto tuple = sorter_->next();
  return tuple;
}

std::vector<SortGroupbyOperator::OwnedValue>
SortGroupbyOperator::key_values(const Tuple &tuple) const {
  std::vector<OwnedValue> values;
  values.reserve(key_positions_.size());
  for (std::size_t position : key_positions_) {
    const Value value = tuple.value_at(position);
    if (std::holds_alternative<std::int32_t>(value))
      values.emplace_back(std::get<std::int32_t>(value));
    else
      values.emplace_back(std::string(std::get<std::string_view>(value)));
  }
  return values;
}

bool SortGroupbyOperator::same_key(const Tuple &tuple,
                                   const std::vector<OwnedValue> &key) {
  for (std::size_t i = 0; i < key_positions_.size(); ++i) {
    ++counter_.comparisons;
    const Value value = tuple.value_at(key_positions_[i]);
    const OwnedValue current =
        std::holds_alternative<std::int32_t>(value)
            ? OwnedValue(std::get<std::int32_t>(value))
            : OwnedValue(std::string(std::get<std::string_view>(value)));
    if (compare_owned(current, key[i]) != 0)
      return false;
  }
  return true;
}

void SortGroupbyOperator::update_aggregates(
    const Tuple &tuple, std::vector<AggregateState> &states) {
  for (std::size_t i = 0; i < aggregates_.size(); ++i) {
    const Aggregate &aggregate = aggregates_[i];
    AggregateState &state = states[i];
    if (aggregate.function == Aggregate::Function::COUNT) {
      ++state.count;
      continue;
    }

    const Value value = tuple.value_at(aggregate_positions_[i]);
    if (aggregate.function == Aggregate::Function::SUM ||
        aggregate.function == Aggregate::Function::AVG) {
      state.sum += std::get<std::int32_t>(value);
      ++state.count;
      continue;
    }

    OwnedValue candidate =
        std::holds_alternative<std::int32_t>(value)
            ? OwnedValue(std::get<std::int32_t>(value))
            : OwnedValue(std::string(std::get<std::string_view>(value)));
    if (!state.initialized ||
        (aggregate.function == Aggregate::Function::MIN &&
         compare_owned(candidate, state.value) < 0) ||
        (aggregate.function == Aggregate::Function::MAX &&
         compare_owned(candidate, state.value) > 0))
      state.value = std::move(candidate);
    state.initialized = true;
  }
}

Tuple SortGroupbyOperator::finish_group(
    const std::vector<OwnedValue> &keys,
    const std::vector<AggregateState> &states) {
  output_record_.clear();
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (std::holds_alternative<std::int32_t>(keys[i]))
      append_int(output_record_, std::get<std::int32_t>(keys[i]));
    else
      append_varchar(output_record_, std::get<std::string>(keys[i]));
  }

  for (std::size_t i = 0; i < aggregates_.size(); ++i) {
    const Aggregate &aggregate = aggregates_[i];
    const AggregateState &state = states[i];
    switch (aggregate.function) {
    case Aggregate::Function::COUNT:
      if (state.count >
          static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
        throw std::overflow_error("COUNT result does not fit in INT!");
      append_int(output_record_, static_cast<std::int32_t>(state.count));
      break;
    case Aggregate::Function::SUM:
      if (state.sum < std::numeric_limits<std::int32_t>::min() ||
          state.sum > std::numeric_limits<std::int32_t>::max())
        throw std::overflow_error("SUM result does not fit in INT!");
      append_int(output_record_, static_cast<std::int32_t>(state.sum));
      break;
    case Aggregate::Function::AVG: {
      const double average =
          static_cast<double>(state.sum) / static_cast<double>(state.count);
      std::ostringstream formatted;
      formatted << std::fixed << std::setprecision(4) << average;
      const std::string text = formatted.str();
      if (text.size() > schema_[keys.size() + i].type.max_length())
        throw std::length_error("AVG result does not fit its output VARCHAR!");
      append_varchar(output_record_, text);
      break;
    }
    case Aggregate::Function::MIN:
    case Aggregate::Function::MAX:
      if (!state.initialized)
        throw std::logic_error("MIN/MAX has no value for a non-empty group!");
      if (std::holds_alternative<std::int32_t>(state.value))
        append_int(output_record_, std::get<std::int32_t>(state.value));
      else
        append_varchar(output_record_, std::get<std::string>(state.value));
      break;
    }
  }
  return switcher::decode(output_record_, schema_);
}

std::optional<Tuple> SortGroupbyOperator::emit_empty_global_group() {
  if (!key_positions_.empty() || empty_global_emitted_)
    return std::nullopt;
  empty_global_emitted_ = true;
  std::vector<AggregateState> states(aggregates_.size());
  for (const Aggregate &aggregate : aggregates_)
    if (aggregate.function != Aggregate::Function::COUNT)
      throw std::runtime_error(
          "Aggregate is undefined for an empty global GROUP BY!");
  std::vector<OwnedValue> no_keys;
  ++counter_.tuples_out;
  return finish_group(no_keys, states);
}

std::optional<Tuple> SortGroupbyOperator::next() {
  if (state_ != State::OPEN)
    throw std::logic_error("Sort GROUP BY can only be queried when open!");
  if (sleepy_)
    return std::nullopt;

  try {
    auto first = next_input();
    if (!first.has_value()) {
      sleepy_ = true;
      return emit_empty_global_group();
    }

    std::vector<OwnedValue> keys = key_values(*first);
    std::vector<AggregateState> states(aggregates_.size());
    update_aggregates(*first, states);
    if (pending_record_.has_value())
      pending_record_.reset();

    while (auto tuple = sorter_->next()) {
      if (!same_key(*tuple, keys)) {
        pending_record_ = switcher::encode(*tuple, sorter_->schema());
        ++counter_.tuples_out;
        return finish_group(keys, states);
      }
      update_aggregates(*tuple, states);
    }
    ++counter_.tuples_out;
    sleepy_ = true;
    return finish_group(keys, states);
  } catch (...) {
    state_ = State::FAILED;
    throw;
  }
}

void SortGroupbyOperator::close() {
  if (state_ != State::OPEN && state_ != State::FAILED)
    throw std::logic_error(
        "Sort GROUP BY can only be closed when open or failed!");

  state_ = State::FAILED;
  if (sorter_needs_close_) {
    sorter_->close();
    sorter_needs_close_ = false;
  }
  pending_record_.reset();
  output_record_.clear();
  state_ = State::CLOSED;
}

Operator::Counter SortGroupbyOperator::counters() const {
  Counter result = sorter_->counters();
  result.tuples_out = counter_.tuples_out;
  result.comparisons += counter_.comparisons;
  return result;
}
} // namespace vql::algebra
