#include "operators/join_inputs.h"

#include "switcher.h"

#include <cerrno>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace vql::algebra::detail {
JoinInputs::JoinInputs(std::unique_ptr<Operator> left,
                       std::unique_ptr<Operator> right,
                       storage::BufferPool &buffer_pool)
    : left_(std::move(left)), right_(std::move(right)),
      buffer_pool_(buffer_pool), schema_(std::vector<Column>{}) {
  if (!left_ || !right_)
    throw std::invalid_argument("Join requires two input operators!");
  schema_ = concat_schemas(left_->schema(), right_->schema());
}

JoinInputs::~JoinInputs() {
  try {
    close();
  } catch (...) {
    // Explicit close reports errors and can be retried; destruction cannot throw.
  }
}

const Schema &JoinInputs::schema() const { return schema_; }
const Schema &JoinInputs::left_schema() const { return left_->schema(); }
const Schema &JoinInputs::right_schema() const { return right_->schema(); }

void JoinInputs::open() {
  if (state_ != State::CREATED)
    throw std::logic_error("Join inputs can only be opened once!");

  state_ = State::FAILED;
  for (std::size_t i = 0; i < schema_.size(); ++i)
    for (std::size_t j = 0; j < i; ++j)
      if (schema_[i].name == schema_[j].name)
        throw std::invalid_argument(
            "Join schema contains duplicate column names!");

  save_input(*left_, left_file_id_, left_needs_close_);
  save_input(*right_, right_file_id_, right_needs_close_);
  state_ = State::OPEN;
}

void JoinInputs::save_input(Operator &input,
                            std::optional<std::size_t> &file_id,
                            bool &needs_close) {
  needs_close = true;
  input.open();
  file_id = create_temp_file();
  storage::TempFile writer(buffer_pool_, *file_id);
  while (auto tuple = input.next())
    append_tuple(writer, *tuple, input.schema());

  input.close();
  needs_close = false;
  remove_file(encoding_file_id_);
}

void JoinInputs::append_tuple(storage::TempFile &writer, const Tuple &tuple,
                              const Schema &schema) {
  const auto before = buffer_pool_.stats();
  try {
    const std::size_t size = switcher::encoded_size(tuple, schema);
    if (size > 4092)
      throw std::length_error(
          "Join input record is too large for a temporary page!");

    if (const auto record = tuple.encoded_record()) {
      writer.append(*record);
    } else {
      if (!encoding_file_id_) {
        encoding_file_id_ = create_temp_file();
        buffer_pool_.new_page(*encoding_file_id_);
      }
      Page &page = buffer_pool_.fetch_page(*encoding_file_id_, 0);
      try {
        switcher::encode_into(tuple, schema, std::span<char>(page));
        writer.append(std::span<const char>(page).first(size));
      } catch (...) {
        buffer_pool_.unpin_page(*encoding_file_id_, 0, true);
        throw;
      }
      buffer_pool_.unpin_page(*encoding_file_id_, 0, true);
    }
  } catch (...) {
    account_io(before);
    throw;
  }
  account_io(before);
}

void JoinInputs::close() {
  if (state_ == State::CLOSED)
    return;
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
  for (auto *file : {&encoding_file_id_, &right_file_id_, &left_file_id_}) {
    try {
      remove_file(*file);
    } catch (...) {
      if (!failure)
        failure = std::current_exception();
    }
  }
  if (failure)
    std::rethrow_exception(failure);
  state_ = State::CLOSED;
}

std::size_t JoinInputs::left_file_id() const {
  if (state_ != State::OPEN)
    throw std::logic_error("Join input files are only available when open!");
  return *left_file_id_;
}

std::size_t JoinInputs::right_file_id() const {
  if (state_ != State::OPEN)
    throw std::logic_error("Join input files are only available when open!");
  return *right_file_id_;
}

Operator::Counter JoinInputs::counters() const { return counter_; }
Operator::Counter JoinInputs::left_counters() const { return left_->counters(); }
Operator::Counter JoinInputs::right_counters() const { return right_->counters(); }

std::size_t JoinInputs::create_temp_file() {
  std::string path =
      (std::filesystem::temp_directory_path() / "vql-join-input-XXXXXX")
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
    return buffer_pool_.open_file(path);
  } catch (...) {
    ::unlink(path.c_str());
    throw;
  }
}

void JoinInputs::remove_file(std::optional<std::size_t> &file_id) {
  if (!file_id)
    return;
  const auto before = buffer_pool_.stats();
  try {
    buffer_pool_.remove_file(*file_id);
    file_id.reset();
  } catch (...) {
    account_io(before);
    throw;
  }
  account_io(before);
}

void JoinInputs::account_io(storage::BufferPool::Stats before) {
  const auto after = buffer_pool_.stats();
  counter_.page_reads += after.reads - before.reads;
  counter_.page_writes += after.writes - before.writes;
}
} // namespace vql::algebra::detail
