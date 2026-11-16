#include "operators/sequential_operator.h"

#include "switcher.h"

#include <stdexcept>

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

  Tuple resi_wit_da_hesi = switcher::decode(bytes.value().bytes, schema_);
  ++counter_.tuples_out;
  return resi_wit_da_hesi;
}

Operator::Counter SequentialOperator::counters() const { return counter_; }

} // namespace vql::algebra
