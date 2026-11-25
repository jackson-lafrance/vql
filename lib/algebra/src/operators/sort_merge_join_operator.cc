#include "operators/sort_merge_join_operator.h"

#include "operators/sort_operator.h"
#include "temp_file.h"

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
SortMergeJoinOperator::SortMergeJoinOperator(
    std::unique_ptr<Operator> left, std::unique_ptr<Operator> right,
    std::vector<Key> keys, storage::BufferPool &buffer_pool,
    std::size_t workspace_frames)
    : buffer_pool_(buffer_pool), keys_(std::move(keys)) {
  if (!left || !right)
    throw std::invalid_argument("SortMergeJoin requires two input operators!");
  if (keys_.empty())
    throw std::invalid_argument("SortMergeJoin requires an equality key!");

  std::vector<SortKey> left_keys;
  std::vector<SortKey> right_keys;
  left_keys.reserve(keys_.size());
  right_keys.reserve(keys_.size());
  for (const Key &key : keys_) {
    left_keys.push_back({key.left, true});
    right_keys.push_back({key.right, true});
  }
  inputs_ = std::make_unique<detail::JoinInputs>(
      std::make_unique<SortOperator>(std::move(left), buffer_pool_,
                                     std::move(left_keys), workspace_frames),
      std::make_unique<SortOperator>(std::move(right), buffer_pool_,
                                     std::move(right_keys), workspace_frames),
      buffer_pool_);
  schema_ = inputs_->schema();
}

SortMergeJoinOperator::~SortMergeJoinOperator() {
  if (state_ == State::OPEN || state_ == State::FAILED) {
    try {
      close();
    } catch (...) {
    }
  }
}

const Schema &SortMergeJoinOperator::schema() const { return schema_; }

void SortMergeJoinOperator::open() {
  if (state_ != State::CREATED)
    throw std::logic_error("SortMergeJoin can only be opened once!");

  state_ = State::FAILED;
  for (const Key &key : keys_)
    if (inputs_->left_schema()[key.left].type.data_type() !=
        inputs_->right_schema()[key.right].type.data_type())
      throw std::invalid_argument(
          "SortMergeJoin equality keys must have the same data type!");

  inputs_->open();
  left_.file_id = inputs_->left_file_id();
  right_.file_id = inputs_->right_file_id();
  left_.page_count = buffer_pool_.num_pages(left_.file_id);
  right_.page_count = buffer_pool_.num_pages(right_.file_id);
  exhausted_ = left_.page_count == 0 || right_.page_count == 0;
  if (!exhausted_) {
    const auto before = buffer_pool_.stats();
    try {
      create_output_page();
      advance(left_);
      advance(right_);
      exhausted_ = !left_.record || !right_.record;
    } catch (...) {
      account_io(before);
      throw;
    }
    account_io(before);
  }
  state_ = State::OPEN;
}

std::optional<Tuple> SortMergeJoinOperator::next() {
  if (state_ != State::OPEN)
    throw std::logic_error("SortMergeJoin can only be queried when open!");
  if (exhausted_)
    return std::nullopt;

  const auto before = buffer_pool_.stats();
  try {
    restore_cursor(left_);
    if (advance_right_) {
      advance(right_);
      advance_right_ = false;
    } else {
      restore_cursor(right_);
    }
    for (;;) {
      if (group_start_) {
        if (right_.record && compare_keys() == 0) {
          Tuple result = emit_match();
          account_io(before);
          return result;
        }

        advance(left_);
        if (left_.record && compare_left_to_output() == 0) {
          rewind_right(*group_start_);
          continue;
        }
        group_start_.reset();
        continue;
      }

      if (!left_.record || !right_.record) {
        exhausted_ = true;
        account_io(before);
        return std::nullopt;
      }
      const int order = compare_keys();
      if (order < 0) {
        advance(left_);
        continue;
      }
      if (order > 0) {
        advance(right_);
        continue;
      }
      group_start_ = right_.current;
      Tuple result = emit_match();
      account_io(before);
      return result;
    }
  } catch (...) {
    state_ = State::FAILED;
    account_io(before);
    throw;
  }
}

void SortMergeJoinOperator::advance(Cursor &cursor) {
  cursor.record.reset();
  cursor.current.reset();
  while (cursor.next.page_id < cursor.page_count) {
    if (!cursor.page)
      cursor.page =
          &buffer_pool_.fetch_page(cursor.file_id, cursor.next.page_id);
    const Position start = cursor.next;
    cursor.record = storage::temp_page::next(*cursor.page, cursor.next.offset);
    if (cursor.record) {
      cursor.current = start;
      return;
    }
    buffer_pool_.unpin_page(cursor.file_id, cursor.next.page_id, false);
    cursor.page = nullptr;
    ++cursor.next.page_id;
    cursor.next.offset = 2;
  }
}

void SortMergeJoinOperator::rewind_right(Position position) {
  close_cursor(right_);
  right_.next = position;
  advance(right_);
}

void SortMergeJoinOperator::unpin_cursor(Cursor &cursor) {
  cursor.record.reset();
  if (cursor.page) {
    buffer_pool_.unpin_page(cursor.file_id, cursor.next.page_id, false);
    cursor.page = nullptr;
  }
}

void SortMergeJoinOperator::restore_cursor(Cursor &cursor) {
  if (cursor.page || !cursor.current)
    return;
  cursor.page = &buffer_pool_.fetch_page(cursor.file_id, cursor.current->page_id);
  std::size_t offset = cursor.current->offset;
  cursor.record = storage::temp_page::next(*cursor.page, offset);
  if (!cursor.record)
    throw std::logic_error("Missing current sort-merge row!");
}

void SortMergeJoinOperator::close_cursor(Cursor &cursor) {
  unpin_cursor(cursor);
  cursor.current.reset();
}

int SortMergeJoinOperator::compare_keys() {
  ++counter_.comparisons;
  const Tuple left(inputs_->left_schema(), *left_.record);
  const Tuple right(inputs_->right_schema(), *right_.record);
  for (const Key &key : keys_)
    if (const int order = compare_values(left[key.left], right[key.right]);
        order != 0)
      return order;
  return 0;
}

int SortMergeJoinOperator::compare_left_to_output() {
  ++counter_.comparisons;
  const Tuple left(inputs_->left_schema(), *left_.record);
  const Tuple previous(
      schema_, std::span<const char>(*output_page_).first(output_size_));
  for (const Key &key : keys_)
    if (const int order = compare_values(left[key.left], previous[key.left]);
        order != 0)
      return order;
  return 0;
}

Tuple SortMergeJoinOperator::emit_match() {
  const auto left = *left_.record;
  const auto right = *right_.record;
  if (left.size() > output_page_->size() ||
      right.size() > output_page_->size() - left.size())
    throw std::length_error(
        "SortMergeJoin match exceeds its 4096-byte output page!");
  std::copy(left.begin(), left.end(), output_page_->begin());
  std::copy(right.begin(), right.end(), output_page_->begin() + left.size());
  output_size_ = left.size() + right.size();
  output_dirty_ = true;
  Tuple result(schema_, std::span<const char>(*output_page_).first(output_size_));
  unpin_cursor(left_);
  unpin_cursor(right_);
  ++counter_.tuples_out;
  advance_right_ = true;
  return result;
}

void SortMergeJoinOperator::close() {
  if (state_ != State::OPEN && state_ != State::FAILED)
    throw std::logic_error("SortMergeJoin must be OPEN or FAILED before close!");

  state_ = State::FAILED;
  exhausted_ = true;
  group_start_.reset();
  advance_right_ = false;
  std::exception_ptr failure;
  const auto before = buffer_pool_.stats();
  for (Cursor *cursor : {&left_, &right_}) {
    try {
      close_cursor(*cursor);
    } catch (...) {
      if (!failure)
        failure = std::current_exception();
    }
  }
  try {
    release_output_page();
  } catch (...) {
    if (!failure)
      failure = std::current_exception();
  }
  account_io(before);
  try {
    inputs_->close();
  } catch (...) {
    if (!failure)
      failure = std::current_exception();
  }
  if (failure)
    std::rethrow_exception(failure);
  state_ = State::CLOSED;
}

Operator::Counter SortMergeJoinOperator::counters() const {
  Counter result = counter_;
  for (const Counter internal : {inputs_->counters(), inputs_->left_counters(),
                                 inputs_->right_counters()}) {
    result.comparisons += internal.comparisons;
    result.page_reads += internal.page_reads;
    result.page_writes += internal.page_writes;
  }
  return result;
}

void SortMergeJoinOperator::create_output_page() {
  std::string path =
      (std::filesystem::temp_directory_path() / "vql-sort-merge-output-XXXXXX")
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

void SortMergeJoinOperator::release_output_page() {
  if (output_page_) {
    buffer_pool_.unpin_page(*output_file_id_, 0, output_dirty_);
    output_page_ = nullptr;
  }
  if (output_file_id_) {
    buffer_pool_.remove_file(*output_file_id_);
    output_file_id_.reset();
  }
}

void SortMergeJoinOperator::account_io(storage::BufferPool::Stats before) {
  const auto after = buffer_pool_.stats();
  counter_.page_reads += after.reads - before.reads;
  counter_.page_writes += after.writes - before.writes;
}
} // namespace vql::algebra
