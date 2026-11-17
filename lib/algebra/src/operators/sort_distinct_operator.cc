#include "operators/sort_distinct_operator.h"

#include "switcher.h"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace vql::algebra {
SortDistinctOperator::SortDistinctOperator(std::unique_ptr<Operator> input,
                                           storage::BufferPool &buffer_pool,
                                           std::size_t workspace_frames)
    : buffer_pool_(buffer_pool) {
  if (!input)
    throw std::invalid_argument("SortDistinct is missing an input operator!");

  schema_ = input->schema();
  std::vector<SortKey> keys;
  keys.reserve(schema_.size());
  for (std::size_t i = 0; i < schema_.size(); ++i)
    keys.push_back({i, true});
  sorter_ = std::make_unique<SortOperator>(std::move(input), buffer_pool_,
                                           std::move(keys), workspace_frames);
}

const Schema &SortDistinctOperator::schema() const { return schema_; }

void SortDistinctOperator::open() {
  if (state_ != State::CREATED)
    throw std::logic_error("SortDistinct can only be opened once!");

  state_ = State::FAILED;
  sorter_needs_close_ = true;
  sorter_->open();
  state_ = State::OPEN;
}

std::optional<Tuple> SortDistinctOperator::next() {
  if (state_ != State::OPEN)
    throw std::logic_error("SortDistinct can only be queried when open!");
  if (sleepy_)
    return std::nullopt;

  try {
    while (auto tuple = sorter_->next()) {
      const auto before = buffer_pool_.stats();
      bool unique;
      try {
        if (!previous_file_id_.has_value())
          create_previous_file();
        Page &previous = buffer_pool_.fetch_page(*previous_file_id_, 0);
        bool dirty = false;
        try {
          unique = true;
          if (previous_size_.has_value()) {
            ++counter_.comparisons;
            const Tuple remembered(
                schema_,
                std::span<const char>(previous).first(*previous_size_));
            unique = !TupleEqual{}(remembered, *tuple);
          }
          if (unique) {
            const std::size_t size = switcher::encode_into(
                *tuple, schema_, std::span<char>(previous));
            previous_size_ = size;
            dirty = true;
          }
        } catch (...) {
          buffer_pool_.unpin_page(*previous_file_id_, 0, true);
          throw;
        }
        buffer_pool_.unpin_page(*previous_file_id_, 0, dirty);
      } catch (...) {
        account_io(before);
        throw;
      }
      account_io(before);

      if (unique) {
        ++counter_.tuples_out;
        return tuple;
      }
    }
    sleepy_ = true;
    return std::nullopt;
  } catch (...) {
    state_ = State::FAILED;
    throw;
  }
}

void SortDistinctOperator::close() {
  if (state_ != State::OPEN && state_ != State::FAILED)
    throw std::logic_error(
        "SortDistinct can only be closed when open or failed!");

  state_ = State::FAILED;
  if (sorter_needs_close_) {
    sorter_->close();
    sorter_needs_close_ = false;
  }

  const auto before = buffer_pool_.stats();
  try {
    if (previous_file_id_.has_value()) {
      buffer_pool_.remove_file(*previous_file_id_);
      previous_file_id_.reset();
    }
  } catch (...) {
    account_io(before);
    throw;
  }
  account_io(before);
  previous_size_.reset();
  sleepy_ = true;
  state_ = State::CLOSED;
}

Operator::Counter SortDistinctOperator::counters() const {
  Counter result = sorter_->counters();
  result.tuples_out = counter_.tuples_out;
  result.comparisons += counter_.comparisons;
  result.page_reads += counter_.page_reads;
  result.page_writes += counter_.page_writes;
  return result;
}

void SortDistinctOperator::create_previous_file() {
  std::string path =
      (std::filesystem::temp_directory_path() / "vql-sort-distinct-XXXXXX")
          .string();
  const int fd = ::mkstemp(path.data());
  if (fd == -1)
    throw std::system_error(errno, std::generic_category(), "mkstemp");
  if (::close(fd) == -1) {
    const int error = errno;
    ::unlink(path.c_str());
    throw std::system_error(error, std::generic_category(), "close");
  }

  try {
    previous_file_id_ = buffer_pool_.open_file(path);
  } catch (...) {
    ::unlink(path.c_str());
    throw;
  }
  buffer_pool_.new_page(*previous_file_id_);
}

void SortDistinctOperator::account_io(storage::BufferPool::Stats before) {
  const auto after = buffer_pool_.stats();
  counter_.page_reads += after.reads - before.reads;
  counter_.page_writes += after.writes - before.writes;
}
} // namespace vql::algebra
