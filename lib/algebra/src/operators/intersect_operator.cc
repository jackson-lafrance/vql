#include "operators/intersect_operator.h"

#include "switcher.h"

#include <algorithm>
#include <cerrno>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace vql::algebra {
IntersectOperator::IntersectOperator(std::unique_ptr<Operator> left_input,
                                     std::unique_ptr<Operator> right_input,
                                     storage::BufferPool &buffer_pool,
                                     std::size_t workspace_frames)
    : buffer_pool_(buffer_pool) {
  if (!left_input || !right_input)
    throw std::invalid_argument("Intersect requires two input operators!");

  validate_set_schemas(left_input->schema(), right_input->schema());
  schema_ = left_input->schema();

  std::vector<SortKey> left_keys;
  std::vector<SortKey> right_keys;
  left_keys.reserve(schema_.size());
  right_keys.reserve(schema_.size());
  for (std::size_t i = 0; i < schema_.size(); ++i) {
    left_keys.push_back({i, true});
    right_keys.push_back({i, true});
  }

  left_ =
      std::make_unique<SortOperator>(std::move(left_input), buffer_pool_,
                                     std::move(left_keys), workspace_frames);
  right_ =
      std::make_unique<SortOperator>(std::move(right_input), buffer_pool_,
                                     std::move(right_keys), workspace_frames);
}

const Schema &IntersectOperator::schema() const { return schema_; }

void IntersectOperator::open() {
  if (state_ != State::CREATED)
    throw std::logic_error("Intersect can only be opened once!");

  state_ = State::FAILED;
  left_needs_close_ = true;
  left_->open();
  right_needs_close_ = true;
  right_->open();

  const auto before = buffer_pool_.stats();
  try {
    create_previous_page();
  } catch (...) {
    account_io(before);
    throw;
  }
  account_io(before);
  state_ = State::OPEN;
}

void IntersectOperator::close() {
  if (state_ != State::OPEN && state_ != State::FAILED)
    throw std::logic_error("Intersect must be OPEN or FAILED before close!");

  state_ = State::FAILED;
  std::exception_ptr failure;
  if (left_needs_close_) {
    try {
      left_->close();
      left_needs_close_ = false;
    } catch (...) {
      failure = std::current_exception();
    }
  }
  if (right_needs_close_) {
    try {
      right_->close();
      right_needs_close_ = false;
    } catch (...) {
      if (!failure)
        failure = std::current_exception();
    }
  }

  const auto before = buffer_pool_.stats();
  try {
    if (previous_page_) {
      buffer_pool_.unpin_page(*previous_file_id_, 0,
                              previous_size_.has_value());
      previous_page_ = nullptr;
    }
    if (previous_file_id_) {
      buffer_pool_.remove_file(*previous_file_id_);
      previous_file_id_.reset();
    }
  } catch (...) {
    if (!failure)
      failure = std::current_exception();
  }
  account_io(before);
  if (failure)
    std::rethrow_exception(failure);
  state_ = State::CLOSED;
}

std::optional<Tuple> IntersectOperator::next() {
  if (state_ != State::OPEN)
    throw std::logic_error("Intersect can only be queried when open!");

  try {
    for (;;) {
      if (!left_tuple_)
        left_tuple_ = left_->next();
      if (!right_tuple_)
        right_tuple_ = right_->next();

      if (previous_size_) {
        if (left_tuple_ && compare_to_previous(*left_tuple_) == 0) {
          left_tuple_ = left_->next();
          continue;
        }
        if (right_tuple_ && compare_to_previous(*right_tuple_) == 0) {
          right_tuple_ = right_->next();
          continue;
        }
      }

      if (!left_tuple_ || !right_tuple_)
        return std::nullopt;

      const int order = compare(*left_tuple_, *right_tuple_);
      if (order < 0) {
        left_tuple_ = left_->next();
        continue;
      }
      if (order > 0) {
        right_tuple_ = right_->next();
        continue;
      }

      remember(*left_tuple_);
      Tuple result(
          schema_,
          std::span<const char>(*previous_page_).first(*previous_size_));
      ++counter_.tuples_out;
      return result;
    }
  } catch (...) {
    state_ = State::FAILED;
    throw;
  }
}

Operator::Counter IntersectOperator::counters() const {
  Counter result = left_->counters();
  const Counter right = right_->counters();
  result.tuples_out = counter_.tuples_out;
  result.comparisons += right.comparisons + counter_.comparisons;
  result.page_reads += right.page_reads + counter_.page_reads;
  result.page_writes += right.page_writes + counter_.page_writes;
  return result;
}

int IntersectOperator::compare(const Tuple &left, const Tuple &right) {
  ++counter_.comparisons;
  for (std::size_t i = 0; i < schema_.size(); ++i) {
    const int result = compare_values(left.value_at(i), right.value_at(i));
    if (result != 0)
      return result;
  }
  return 0;
}

int IntersectOperator::compare_to_previous(const Tuple &tuple) {
  ++counter_.comparisons;
  const Tuple previous(
      schema_, std::span<const char>(*previous_page_).first(*previous_size_));
  for (std::size_t i = 0; i < schema_.size(); ++i) {
    const int result = compare_values(tuple.value_at(i), previous.value_at(i));
    if (result != 0)
      return result;
  }
  return 0;
}

void IntersectOperator::remember(const Tuple &tuple) {
  const auto record = tuple.encoded_record();
  if (!record)
    throw std::logic_error("Intersect cannot store a projected tuple!");
  if (record->size() > previous_page_->size())
    throw std::length_error("Intersect tuple exceeds its output page!");
  std::copy(record->begin(), record->end(), previous_page_->begin());
  previous_size_ = record->size();
}

void IntersectOperator::create_previous_page() {
  std::string path =
      (std::filesystem::temp_directory_path() / "vql-intersect-previous-XXXXXX")
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
  try {
    buffer_pool_.new_page(*previous_file_id_);
    previous_page_ = &buffer_pool_.fetch_page(*previous_file_id_, 0);
  } catch (...) {
    buffer_pool_.remove_file(*previous_file_id_);
    previous_file_id_.reset();
    throw;
  }
}

void IntersectOperator::account_io(storage::BufferPool::Stats before) {
  const auto after = buffer_pool_.stats();
  counter_.page_reads += after.reads - before.reads;
  counter_.page_writes += after.writes - before.writes;
}
} // namespace vql::algebra
