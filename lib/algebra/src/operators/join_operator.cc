#include "operators/join_operator.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace vql::algebra {
JoinOperator::JoinOperator(std::unique_ptr<Operator> left,
                           std::unique_ptr<Operator> right,
                           std::function<bool(const Tuple &)> condition,
                           storage::BufferPool &buffer_pool,
                           std::size_t workspace_frames)
    : inputs_(std::move(left), std::move(right), buffer_pool),
      buffer_pool_(buffer_pool), condition_(std::move(condition)),
      workspace_frames_(workspace_frames) {
  if (!condition_)
    throw std::invalid_argument("Join is missing a condition!");
  if (workspace_frames_ < 3)
    throw std::invalid_argument("Join requires at least three workspace frames!");
  schema_ = inputs_.schema();
}

JoinOperator::~JoinOperator() {
  if (state_ == State::OPEN || state_ == State::FAILED) {
    try {
      close();
    } catch (...) {
    }
  }
}

const Schema &JoinOperator::schema() const { return schema_; }

void JoinOperator::open() {
  if (state_ != State::CREATED)
    throw std::logic_error("Join can only be opened once!");

  state_ = State::FAILED;
  inputs_.open();
  left_file_id_ = inputs_.left_file_id();
  right_file_id_ = inputs_.right_file_id();
  left_page_count_ = buffer_pool_.num_pages(left_file_id_);
  exhausted_ = left_page_count_ == 0 ||
               buffer_pool_.num_pages(right_file_id_) == 0;

  if (!exhausted_) {
    const auto before = buffer_pool_.stats();
    try {
      create_output_page();
      load_outer_block();
    } catch (...) {
      account_io(before);
      throw;
    }
    account_io(before);
  }
  state_ = State::OPEN;
}

std::optional<Tuple> JoinOperator::next() {
  if (state_ != State::OPEN)
    throw std::logic_error("Join can only be queried when open!");
  if (exhausted_)
    return std::nullopt;

  const auto before = buffer_pool_.stats();
  try {
    pin_outer_block();
    std::optional<std::span<const char>> right_record;
    if (has_right_record_)
      right_record = right_reader_->current();

    for (;;) {
      if (!right_record) {
        right_record = right_reader_->next();
        if (!right_record) {
          right_reader_.reset();
          release_outer_block();
          if (!load_outer_block()) {
            exhausted_ = true;
            account_io(before);
            return std::nullopt;
          }
          continue;
        }
      }

      const auto left_record = next_outer_record();
      if (!left_record) {
        right_record.reset();
        outer_page_index_ = 0;
        outer_position_ = 2;
        continue;
      }

      Tuple candidate = make_candidate(*left_record, *right_record);
      ++counter_.comparisons;
      if (condition_(candidate)) {
        unpin_outer_block();
        right_reader_->release_page();
        has_right_record_ = true;
        ++counter_.tuples_out;
        account_io(before);
        return candidate;
      }
    }
  } catch (...) {
    state_ = State::FAILED;
    account_io(before);
    throw;
  }
}

bool JoinOperator::load_outer_block() {
  if (next_left_page_ == left_page_count_)
    return false;

  const std::size_t count = std::min(workspace_frames_ - 2,
                                    left_page_count_ - next_left_page_);
  outer_pages_.reserve(count);
  for (std::size_t i = 0; i < count; ++i)
    outer_pages_.push_back({next_left_page_++, nullptr});
  pin_outer_block();
  outer_page_index_ = 0;
  outer_position_ = 2;
  right_reader_ =
      std::make_unique<storage::TempFileReader>(buffer_pool_, right_file_id_);
  return true;
}

void JoinOperator::pin_outer_block() {
  for (auto &outer : outer_pages_)
    if (outer.page == nullptr)
      outer.page = &buffer_pool_.fetch_page(left_file_id_, outer.page_id);
}

void JoinOperator::unpin_outer_block() {
  for (auto &outer : outer_pages_) {
    if (outer.page != nullptr) {
      buffer_pool_.unpin_page(left_file_id_, outer.page_id, false);
      outer.page = nullptr;
    }
  }
}

void JoinOperator::release_outer_block() {
  unpin_outer_block();
  outer_pages_.clear();
}

std::optional<std::span<const char>> JoinOperator::next_outer_record() {
  while (outer_page_index_ < outer_pages_.size()) {
    const auto record = storage::temp_page::next(
        *outer_pages_[outer_page_index_].page, outer_position_);
    if (record)
      return record;
    ++outer_page_index_;
    outer_position_ = 2;
  }
  return std::nullopt;
}

Tuple JoinOperator::make_candidate(std::span<const char> left,
                                    std::span<const char> right) {
  if (left.size() > output_page_->size() ||
      right.size() > output_page_->size() - left.size())
    throw std::length_error("Join candidate exceeds its 4096-byte output page!");

  std::copy(left.begin(), left.end(), output_page_->begin());
  std::copy(right.begin(), right.end(), output_page_->begin() + left.size());
  output_dirty_ = true;
  return Tuple(schema_, std::span<const char>(*output_page_)
                            .first(left.size() + right.size()));
}

void JoinOperator::close() {
  if (state_ != State::OPEN && state_ != State::FAILED)
    throw std::logic_error("Join must be OPEN or FAILED before close!");

  state_ = State::FAILED;
  exhausted_ = true;
  std::exception_ptr failure;
  const auto before = buffer_pool_.stats();
  if (right_reader_) {
    try {
      right_reader_->close();
      right_reader_.reset();
    } catch (...) {
      failure = std::current_exception();
    }
  }
  try {
    release_outer_block();
  } catch (...) {
    if (!failure)
      failure = std::current_exception();
  }
  try {
    release_output_page();
  } catch (...) {
    if (!failure)
      failure = std::current_exception();
  }
  account_io(before);

  try {
    inputs_.close();
  } catch (...) {
    if (!failure)
      failure = std::current_exception();
  }
  if (failure)
    std::rethrow_exception(failure);
  state_ = State::CLOSED;
}

Operator::Counter JoinOperator::counters() const {
  Counter result = counter_;
  const Counter saved = inputs_.counters();
  result.page_reads += saved.page_reads;
  result.page_writes += saved.page_writes;
  return result;
}

void JoinOperator::create_output_page() {
  std::string path =
      (std::filesystem::temp_directory_path() / "vql-join-output-XXXXXX")
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
    output_file_id_ = buffer_pool_.open_file(path);
  } catch (...) {
    ::unlink(path.c_str());
    throw;
  }
  buffer_pool_.new_page(*output_file_id_);
  output_page_ = &buffer_pool_.fetch_page(*output_file_id_, 0);
}

void JoinOperator::release_output_page() {
  if (output_page_) {
    buffer_pool_.unpin_page(*output_file_id_, 0, output_dirty_);
    output_page_ = nullptr;
  }
  if (output_file_id_) {
    buffer_pool_.remove_file(*output_file_id_);
    output_file_id_.reset();
  }
}

void JoinOperator::account_io(storage::BufferPool::Stats before) {
  const auto after = buffer_pool_.stats();
  counter_.page_reads += after.reads - before.reads;
  counter_.page_writes += after.writes - before.writes;
}
} // namespace vql::algebra
