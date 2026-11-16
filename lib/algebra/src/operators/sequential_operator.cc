#include "operators/sequential_operator.h"

#include <bit>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vql::algebra {

SequentialOperator::SequentialOperator(storage::BufferPool &buffer_pool,
                                       std::size_t file_id, Schema schema)
    : buffer_pool_(buffer_pool), file_id_(file_id) {
  schema_ = schema;
}

const Schema &SequentialOperator::schema() const { return schema_; }

void SequentialOperator::open() {
  if (state_ != State::CREATED)
    throw std::logic_error("SeqScan can only be opened once!");

  state_ = State::FAILED;
  heap_scan_ = std::make_unique<storage::HeapScan>(buffer_pool_, file_id_);
  state_ = State::OPEN;
}

void SequentialOperator::close() {
  if (state_ != State::OPEN && state_ != State::FAILED)
    throw std::logic_error(
        "SeqScan must be open or failed before it can be closed!");

  if (heap_scan_)
    heap_scan_->close();

  state_ = State::CLOSED;
}

std::optional<Tuple> SequentialOperator::next() {
  if (state_ != State::OPEN)
    throw std::logic_error("SeqScan can only be queried when open!");

  auto before = buffer_pool_.stats();
  std::optional<storage::ScannedRecord> bytes = heap_scan_->next();
  auto after = buffer_pool_.stats();

  counter_.page_reads += after.reads - before.reads;
  counter_.page_writes += after.writes - before.writes;

  if (!bytes.has_value())
    return std::nullopt;

  std::span<const char> record = bytes.value().bytes;
  std::size_t position = 0;
  std::vector<Value> values;
  values.reserve(schema_.size());

  for (std::size_t i{0}; i < schema_.size(); ++i) {
    if (schema_[i].type.data_type() == DataType::INT) {
      if (position > record.size() || record.size() - position < 4)
        throw std::runtime_error("INT too short!");
      values.emplace_back(
          std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(
              static_cast<unsigned char>(record[position]) |
              (static_cast<unsigned char>(record[position + 1]) << 8) |
              (static_cast<unsigned char>(record[position + 2]) << 16) |
              (static_cast<unsigned char>(record[position + 3]) << 24))));
      position += 4;
    } else {
      if (position > record.size() || record.size() - position < 2)
        throw std::runtime_error("VARCHAR too short!");

      std::uint16_t length =
          static_cast<unsigned char>(record[position]) |
          (static_cast<unsigned char>(record[position + 1]) << 8);
      position += 2;

      if (length > record.size() - position ||
          length > schema_[i].type.max_length())
        throw std::runtime_error("Malformed VARCHAR!");

      values.emplace_back(std::string(record.data() + position, length));
      position += length;
    }
  }

  if (position != record.size())
    throw std::runtime_error("Unexpected bytes after record!");

  Tuple resi_wit_da_hesi(std::move(values));
  ++counter_.tuples_out;
  return resi_wit_da_hesi;
}

Operator::Counter SequentialOperator::counters() const { return counter_; }

} // namespace vql::algebra
