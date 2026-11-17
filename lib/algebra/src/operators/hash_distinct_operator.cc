#include "operators/hash_distinct_operator.h"

#include "switcher.h"
#include "temp_file.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace vql::algebra {

HashDistinctOperator::HashDistinctOperator(std::unique_ptr<Operator> input,
                                           storage::BufferPool &buffer_pool,
                                           std::size_t partition_count)
    : input_(std::move(input)), buffer_pool_(buffer_pool),
      partition_count_(partition_count) {
  if (!input_)
    throw std::invalid_argument("HashDistinct is missing an input operator!");
  if (partition_count_ == 0)
    throw std::invalid_argument(
        "HashDistinct requires at least one partition!");

  schema_ = input_->schema();
}

const Schema &HashDistinctOperator::schema() const { return schema_; }

void HashDistinctOperator::open() {
  if (state_ != State::CREATED)
    throw std::logic_error("HashDistinct can only be opened once!");

  state_ = State::FAILED;
  for (std::size_t i = 0; i < schema_.size(); ++i)
    for (std::size_t j = 0; j < i; ++j)
      if (schema_[i].name == schema_[j].name)
        throw std::invalid_argument(
            "HashDistinct schema contains duplicate column names!");

  partition_input();
  sleepy_ = !prepare_next_batch();
  state_ = State::OPEN;
}

void HashDistinctOperator::close() {
  if (state_ != State::OPEN && state_ != State::FAILED)
    throw std::logic_error(
        "HashDistinct can only be closed when open or failed!");

  state_ = State::FAILED;
  if (input_needs_close_) {
    input_->close();
    input_needs_close_ = false;
  }

  if (batch_page_ != nullptr) {
    buffer_pool_.unpin_page(batch_file_id_.value(), 0, true);
    batch_page_ = nullptr;
  }

  while (!owned_files_.empty())
    remove_temp_file(owned_files_.back());

  pending_files_.clear();
  batch_file_id_.reset();
  sleepy_ = true;
  state_ = State::CLOSED;
}

std::optional<Tuple> HashDistinctOperator::next() {
  if (state_ != State::OPEN)
    throw std::logic_error("HashDistinct can only be queried when open!");

  try {
    while (!sleepy_) {
      auto record = storage::temp_page::next(*batch_page_, output_position);
      if (record.has_value()) {
        Tuple result = switcher::decode(*record, schema_);
        ++counter_.tuples_out;
        return result;
      }

      sleepy_ = !prepare_next_batch();
    }
    return std::nullopt;
  } catch (...) {
    state_ = State::FAILED;
    throw;
  }
}

Operator::Counter HashDistinctOperator::counters() const { return counter_; }

std::size_t HashDistinctOperator::create_temp_file() {
  owned_files_.reserve(owned_files_.size() + 1);
  std::string path =
      (std::filesystem::temp_directory_path() / "vql-distinct-XXXXXX").string();
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

void HashDistinctOperator::remove_temp_file(std::size_t file_id) {
  buffer_pool_.remove_file(file_id);
  std::erase(owned_files_, file_id);
}

bool HashDistinctOperator::batch_contains(std::span<const char> record) {
  if (batch_page_ == nullptr)
    throw std::logic_error("HashDistinct batch page is not pinned!");

  std::size_t position = 2;
  while (auto stored = storage::temp_page::next(*batch_page_, position)) {
    ++counter_.comparisons;
    if (stored->size() == record.size() &&
        std::equal(stored->begin(), stored->end(), record.begin()))
      return true;
  }
  return false;
}

void HashDistinctOperator::partition_input() {
  std::vector<std::optional<std::size_t>> partitions(partition_count_);

  input_needs_close_ = true;
  input_->open();
  while (auto tuple = input_->next()) {
    const std::size_t size = switcher::encoded_size(*tuple, schema_);
    if (size > 4092)
      throw std::length_error("Record is too large for a temporary page!");
    const std::size_t partition = TupleHash{}(*tuple) % partition_count_;

    const auto before = buffer_pool_.stats();
    try {
      if (!partitions[partition].has_value()) {
        const std::size_t file_id = create_temp_file();
        partitions[partition] = file_id;
        pending_files_.push_back(file_id);
      }
      if (!batch_file_id_.has_value()) {
        batch_file_id_ = create_temp_file();
        buffer_pool_.new_page(*batch_file_id_);
      }
      Page &staging = buffer_pool_.fetch_page(*batch_file_id_, 0);
      try {
        switcher::encode_into(*tuple, schema_,
                              std::span<char>(staging).first(size));
        storage::TempFile writer(buffer_pool_, *partitions[partition]);
        writer.append(std::span<const char>(staging).first(size));
      } catch (...) {
        buffer_pool_.unpin_page(*batch_file_id_, 0, true);
        throw;
      }
      buffer_pool_.unpin_page(*batch_file_id_, 0, true);
    } catch (...) {
      const auto after = buffer_pool_.stats();
      counter_.page_reads += after.reads - before.reads;
      counter_.page_writes += after.writes - before.writes;
      throw;
    }
    const auto after = buffer_pool_.stats();
    counter_.page_reads += after.reads - before.reads;
    counter_.page_writes += after.writes - before.writes;
  }
  input_->close();
  input_needs_close_ = false;
}

bool HashDistinctOperator::prepare_next_batch() {
  const auto before = buffer_pool_.stats();
  bool ready = false;
  try {
    while (!pending_files_.empty() && !ready) {
      if (!batch_file_id_.has_value()) {
        batch_file_id_ = create_temp_file();
        buffer_pool_.new_page(*batch_file_id_);
      }
      if (batch_page_ == nullptr)
        batch_page_ = &buffer_pool_.fetch_page(*batch_file_id_, 0);

      storage::temp_page::initialize(*batch_page_);
      output_position = 2;
      bool frozen = false;
      std::optional<std::size_t> remainder;
      const std::size_t source = pending_files_.back();
      storage::TempFileReader reader(buffer_pool_, source);

      while (auto record = reader.next()) {
        if (batch_contains(*record))
          continue;
        if (!frozen && storage::temp_page::append(*batch_page_, *record))
          continue;

        if (storage::temp_page::used_end(*batch_page_) == 2)
          throw std::length_error(
              "HashDistinct cannot fit one record in its batch!");

        frozen = true;
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
      ready = storage::temp_page::used_end(*batch_page_) != 2;
    }

    if (!ready && batch_page_ != nullptr) {
      buffer_pool_.unpin_page(*batch_file_id_, 0, true);
      batch_page_ = nullptr;
    }
  } catch (...) {
    if (batch_page_ != nullptr) {
      buffer_pool_.unpin_page(*batch_file_id_, 0, true);
      batch_page_ = nullptr;
    }
    const auto after = buffer_pool_.stats();
    counter_.page_reads += after.reads - before.reads;
    counter_.page_writes += after.writes - before.writes;
    throw;
  }
  const auto after = buffer_pool_.stats();
  counter_.page_reads += after.reads - before.reads;
  counter_.page_writes += after.writes - before.writes;
  return ready;
}

} // namespace vql::algebra
