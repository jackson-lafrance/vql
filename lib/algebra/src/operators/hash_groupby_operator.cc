#include "operators/hash_groupby_operator.h"

#include "switcher.h"
#include "temp_file.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace vql::algebra {
namespace {
constexpr std::size_t kGroupBatchBudget = 4096;

int compare_owned(const std::variant<std::int32_t, std::string> &left,
                  const std::variant<std::int32_t, std::string> &right) {
  if (left.index() != right.index())
    throw std::logic_error("Hash GROUP BY compared different value types!");
  if (std::holds_alternative<std::int32_t>(left)) {
    const auto a = std::get<std::int32_t>(left);
    const auto b = std::get<std::int32_t>(right);
    return a < b ? -1 : a > b ? 1 : 0;
  }
  const int result = std::get<std::string>(left).compare(
      std::get<std::string>(right));
  return result < 0 ? -1 : result > 0 ? 1 : 0;
}

std::string function_name(GroupbyOperator::Aggregate::Function function) {
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

void append_int(std::vector<char> &record, std::int32_t value) {
  const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
  for (unsigned shift = 0; shift < 32; shift += 8)
    record.push_back(static_cast<char>((bits >> shift) & 0xFF));
}

void append_varchar(std::vector<char> &record, std::string_view value) {
  if (value.size() > std::numeric_limits<std::uint16_t>::max())
    throw std::length_error("Hash GROUP BY output VARCHAR is too long!");
  const auto length = static_cast<std::uint16_t>(value.size());
  record.push_back(static_cast<char>(length & 0xFF));
  record.push_back(static_cast<char>((length >> 8) & 0xFF));
  record.insert(record.end(), value.begin(), value.end());
}

void append_key_int(std::string &key, std::int32_t value) {
  const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
  for (unsigned shift = 0; shift < 32; shift += 8)
    key.push_back(static_cast<char>((bits >> shift) & 0xFF));
}
} // namespace

HashGroupbyOperator::HashGroupbyOperator(
    std::unique_ptr<Operator> input, storage::BufferPool &buffer_pool,
    std::size_t partition_count, std::vector<std::string> key_names,
    std::vector<Aggregate> aggregates)
    : input_(std::move(input)), buffer_pool_(buffer_pool),
      partition_count_(partition_count), key_names_(std::move(key_names)),
      aggregates_(std::move(aggregates)) {
  if (!input_)
    throw std::invalid_argument("Hash GROUP BY is missing an input operator!");
  if (partition_count_ == 0)
    throw std::invalid_argument(
        "Hash GROUP BY requires at least one partition!");

  const Schema &input_schema = input_->schema();
  key_positions_.reserve(key_names_.size());
  std::vector<Column> output_columns;
  output_columns.reserve(key_names_.size() + aggregates_.size());
  for (const std::string &name : key_names_) {
    const std::size_t position = input_schema.index_of(name);
    key_positions_.push_back(position);
    output_columns.push_back(input_schema[position]);
  }

  aggregate_positions_.reserve(aggregates_.size());
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
    const std::string output_name = aggregate.output_name.empty()
                                        ? default_name
                                        : aggregate.output_name;
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
}

const Schema &HashGroupbyOperator::schema() const { return schema_; }

void HashGroupbyOperator::open() {
  if (state_ != State::CREATED)
    throw std::logic_error("Hash GROUP BY can only be opened once!");

  state_ = State::FAILED;
  partition_input();
  state_ = State::OPEN;
}

void HashGroupbyOperator::close() {
  if (state_ != State::OPEN && state_ != State::FAILED)
    throw std::logic_error(
        "Hash GROUP BY can only be closed when open or failed!");

  state_ = State::FAILED;
  if (input_needs_close_) {
    input_->close();
    input_needs_close_ = false;
  }
  while (!owned_files_.empty())
    remove_temp_file(owned_files_.back());
  pending_files_.clear();
  groups_.clear();
  group_index_.clear();
  output_record_.clear();
  sleepy_ = true;
  state_ = State::CLOSED;
}

std::optional<Tuple> HashGroupbyOperator::next() {
  if (state_ != State::OPEN)
    throw std::logic_error("Hash GROUP BY can only be queried when open!");
  if (sleepy_)
    return std::nullopt;

  try {
    for (;;) {
      if (output_position_ < groups_.size()) {
        Tuple result = finish_group(groups_[output_position_++]);
        ++counter_.tuples_out;
        return result;
      }
      if (!prepare_next_batch()) {
        sleepy_ = true;
        return emit_empty_global_group();
      }
      output_position_ = 0;
    }
  } catch (...) {
    state_ = State::FAILED;
    throw;
  }
}

Operator::Counter HashGroupbyOperator::counters() const { return counter_; }

void HashGroupbyOperator::partition_input() {
  std::vector<std::optional<std::size_t>> partitions(partition_count_);
  input_needs_close_ = true;
  input_->open();
  while (auto tuple = input_->next()) {
    const std::size_t size = switcher::encoded_size(*tuple, input_->schema());
    if (size > 4092)
      throw std::length_error("GROUP BY tuple is too large for a temp page!");
    const std::string key = make_group_key(*tuple);
    const std::size_t partition = std::hash<std::string>{}(key) % partition_count_;

    const auto before = buffer_pool_.stats();
    try {
      if (!partitions[partition].has_value()) {
        const std::size_t file_id = create_temp_file();
        partitions[partition] = file_id;
        pending_files_.push_back(file_id);
      }
      if (!staging_file_id_.has_value()) {
        staging_file_id_ = create_temp_file();
        buffer_pool_.new_page(*staging_file_id_);
      }
      Page &staging = buffer_pool_.fetch_page(*staging_file_id_, 0);
      try {
        switcher::encode_into(*tuple, input_->schema(),
                              std::span<char>(staging).first(size));
        storage::TempFile writer(buffer_pool_, *partitions[partition]);
        writer.append(std::span<const char>(staging).first(size));
      } catch (...) {
        buffer_pool_.unpin_page(*staging_file_id_, 0, true);
        throw;
      }
      buffer_pool_.unpin_page(*staging_file_id_, 0, true);
    } catch (...) {
      account_io(before);
      throw;
    }
    account_io(before);
  }
  input_->close();
  input_needs_close_ = false;
}

bool HashGroupbyOperator::prepare_next_batch() {
  const auto before = buffer_pool_.stats();
  try {
    while (!pending_files_.empty()) {
      groups_.clear();
      group_index_.clear();
      group_memory_used_ = 0;
      output_position_ = 0;
      std::optional<std::size_t> remainder;
      const std::size_t source = pending_files_.back();
      storage::TempFileReader reader(buffer_pool_, source);

      while (auto record = reader.next()) {
        Tuple tuple = switcher::decode(*record, input_->schema());
        std::string key = make_group_key(tuple);
        ++counter_.comparisons;
        auto existing = group_index_.find(key);
        if (existing != group_index_.end()) {
          update_aggregates(tuple, groups_[existing->second]);
          continue;
        }

        const std::size_t estimate = estimate_group_bytes(tuple, key);
        if (estimate <= kGroupBatchBudget -
                            std::min(group_memory_used_, kGroupBatchBudget)) {
          GroupState group;
          group.keys = key_values(tuple);
          group.aggregates.resize(aggregates_.size());
          group.memory_estimate = estimate;
          update_aggregates(tuple, group);
          const std::size_t position = groups_.size();
          groups_.push_back(std::move(group));
          group_index_.emplace(std::move(key), position);
          group_memory_used_ += estimate;
          continue;
        }

        if (groups_.empty())
          throw std::length_error(
              "One hash GROUP BY state exceeds the one-page batch budget!");
        if (!remainder.has_value())
          remainder = create_temp_file();
        storage::TempFile writer(buffer_pool_, *remainder);
        writer.append(*record);
      }
      reader.close();
      remove_temp_file(source);
      pending_files_.pop_back();
      if (remainder.has_value())
        pending_files_.push_back(*remainder);
      if (!groups_.empty())
        return true;
    }
  } catch (...) {
    account_io(before);
    throw;
  }
  account_io(before);
  return false;
}

std::size_t HashGroupbyOperator::create_temp_file() {
  owned_files_.reserve(owned_files_.size() + 1);
  std::string path =
      (std::filesystem::temp_directory_path() / "vql-groupby-XXXXXX").string();
  const int fd = ::mkstemp(path.data());
  if (fd == -1)
    throw std::system_error(errno, std::generic_category(), "mkstemp");
  if (::close(fd) == -1) {
    const int error = errno;
    ::unlink(path.c_str());
    throw std::system_error(error, std::generic_category(), "close");
  }

  std::size_t file_id;
  try {
    file_id = buffer_pool_.open_file(path);
  } catch (...) {
    ::unlink(path.c_str());
    throw;
  }
  owned_files_.push_back(file_id);
  return file_id;
}

void HashGroupbyOperator::remove_temp_file(std::size_t file_id) {
  buffer_pool_.remove_file(file_id);
  std::erase(owned_files_, file_id);
}

std::string HashGroupbyOperator::make_group_key(const Tuple &tuple) const {
  std::string key;
  for (std::size_t position : key_positions_) {
    const Value value = tuple.value_at(position);
    if (std::holds_alternative<std::int32_t>(value)) {
      append_key_int(key, std::get<std::int32_t>(value));
    } else {
      const std::string_view text = std::get<std::string_view>(value);
      if (text.size() > std::numeric_limits<std::uint16_t>::max())
        throw std::length_error("GROUP BY key is too long!");
      const auto length = static_cast<std::uint16_t>(text.size());
      key.push_back(static_cast<char>(length & 0xFF));
      key.push_back(static_cast<char>((length >> 8) & 0xFF));
      key.append(text);
    }
  }
  return key;
}

std::vector<HashGroupbyOperator::OwnedValue>
HashGroupbyOperator::key_values(const Tuple &tuple) const {
  std::vector<OwnedValue> keys;
  keys.reserve(key_positions_.size());
  for (std::size_t position : key_positions_) {
    const Value value = tuple.value_at(position);
    if (std::holds_alternative<std::int32_t>(value))
      keys.emplace_back(std::get<std::int32_t>(value));
    else
      keys.emplace_back(std::string(std::get<std::string_view>(value)));
  }
  return keys;
}

std::size_t HashGroupbyOperator::estimate_group_bytes(
    const Tuple &tuple, std::string_view encoded_key) const {
  std::size_t bytes = 128 + encoded_key.size() +
                      key_positions_.size() * sizeof(OwnedValue) +
                      aggregates_.size() * sizeof(AggregateState);
  for (std::size_t position : key_positions_)
    if (tuple.schema()[position].type.data_type() == DataType::VARCHAR)
      bytes += std::get<std::string_view>(tuple.value_at(position)).size();
  for (std::size_t i = 0; i < aggregates_.size(); ++i) {
    const auto function = aggregates_[i].function;
    if ((function == Aggregate::Function::MIN ||
         function == Aggregate::Function::MAX) &&
        input_->schema()[aggregate_positions_[i]].type.data_type() ==
            DataType::VARCHAR)
      bytes += input_->schema()[aggregate_positions_[i]].type.max_length();
  }
  return bytes;
}

void HashGroupbyOperator::update_aggregates(const Tuple &tuple,
                                            GroupState &group) {
  for (std::size_t i = 0; i < aggregates_.size(); ++i) {
    const Aggregate &aggregate = aggregates_[i];
    AggregateState &state = group.aggregates[i];
    if (aggregate.function == Aggregate::Function::COUNT) {
      if (state.count == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("COUNT state overflow!");
      ++state.count;
      continue;
    }

    const Value value = tuple.value_at(aggregate_positions_[i]);
    if (aggregate.function == Aggregate::Function::SUM ||
        aggregate.function == Aggregate::Function::AVG) {
      const std::int64_t add = std::get<std::int32_t>(value);
      if ((add > 0 && state.sum > std::numeric_limits<std::int64_t>::max() - add) ||
          (add < 0 && state.sum < std::numeric_limits<std::int64_t>::min() - add))
        throw std::overflow_error("SUM/AVG accumulator overflow!");
      state.sum += add;
      if (aggregate.function == Aggregate::Function::AVG) {
        if (state.count == std::numeric_limits<std::uint64_t>::max())
          throw std::overflow_error("AVG count overflow!");
        ++state.count;
      }
      continue;
    }

    OwnedValue candidate = std::holds_alternative<std::int32_t>(value)
                               ? OwnedValue(std::get<std::int32_t>(value))
                               : OwnedValue(std::string(
                                     std::get<std::string_view>(value)));
    if (!state.initialized ||
        (aggregate.function == Aggregate::Function::MIN &&
         compare_owned(candidate, state.value) < 0) ||
        (aggregate.function == Aggregate::Function::MAX &&
         compare_owned(candidate, state.value) > 0))
      state.value = std::move(candidate);
    state.initialized = true;
  }
}

Tuple HashGroupbyOperator::finish_group(const GroupState &group) {
  output_record_.clear();
  for (std::size_t i = 0; i < group.keys.size(); ++i) {
    if (std::holds_alternative<std::int32_t>(group.keys[i]))
      append_int(output_record_, std::get<std::int32_t>(group.keys[i]));
    else
      append_varchar(output_record_, std::get<std::string>(group.keys[i]));
  }

  for (std::size_t i = 0; i < aggregates_.size(); ++i) {
    const Aggregate &aggregate = aggregates_[i];
    const AggregateState &state = group.aggregates[i];
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
      const double average = static_cast<double>(state.sum) /
                             static_cast<double>(state.count);
      std::ostringstream formatted;
      formatted << std::fixed << std::setprecision(4) << average;
      const std::string text = formatted.str();
      if (text.size() > schema_[group.keys.size() + i].type.max_length())
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

std::optional<Tuple> HashGroupbyOperator::emit_empty_global_group() {
  if (!key_positions_.empty() || empty_global_emitted_ || counter_.tuples_out != 0)
    return std::nullopt;
  empty_global_emitted_ = true;
  GroupState empty;
  empty.aggregates.resize(aggregates_.size());
  for (const Aggregate &aggregate : aggregates_)
    if (aggregate.function != Aggregate::Function::COUNT)
      throw std::runtime_error(
          "Aggregate is undefined for an empty global GROUP BY!");
  ++counter_.tuples_out;
  return finish_group(empty);
}

void HashGroupbyOperator::account_io(storage::BufferPool::Stats before) {
  const auto after = buffer_pool_.stats();
  counter_.page_reads += after.reads - before.reads;
  counter_.page_writes += after.writes - before.writes;
}
} // namespace vql::algebra
