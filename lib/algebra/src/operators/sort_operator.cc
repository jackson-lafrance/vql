#include "operators/sort_operator.h"
#include "help_me_god.h"
#include "switcher.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace vql::algebra {
SortOperator::SortOperator(std::unique_ptr<Operator> input,
                           storage::BufferPool &buffer_pool,
                           std::vector<SortKey> keys,
                           std::size_t workspace_frames)
    : input_(std::move(input)), buffer_pool_(buffer_pool),
      keys_(std::move(keys)), workspace_frames_(workspace_frames) {
  if (!input_)
    throw std::invalid_argument("Sort is missing an input operator!");
  if (workspace_frames_ < 3)
    throw std::invalid_argument(
        "Sort requires at least three workspace frames!");

  schema_ = input_->schema();
  for (const SortKey &key : keys_)
    if (key.position >= schema_.size())
      throw std::out_of_range("Sort key position is out of range!");
}

const Schema &SortOperator::schema() const { return schema_; }

void SortOperator::open() {
  if (state_ != State::CREATED)
    throw std::logic_error("Sort can only be opened once!");

  state_ = State::FAILED;
  for (std::size_t i = 0; i < schema_.size(); ++i)
    for (std::size_t j = 0; j < i; ++j)
      if (schema_[i].name == schema_[j].name)
        throw std::invalid_argument(
            "Sort schema contains duplicate column names!");

  input_needs_close_ = true;
  input_->open();
  generate_runs();
  input_->close();
  input_needs_close_ = false;

  const auto before = buffer_pool_.stats();
  try {
    merge_runs();
  } catch (...) {
    account_io(before);
    throw;
  }
  account_io(before);
  state_ = State::OPEN;
}

std::optional<Tuple> SortOperator::next() {
  if (state_ != State::OPEN)
    throw std::logic_error("Sort can only be queried when open!");

  const auto before = buffer_pool_.stats();
  try {
    if (!final_merge_prepared_) {
      prepare_merge(run_files_);
      final_merge_prepared_ = true;
    }
    if (pending_advance_.has_value()) {
      MergeInput &previous = merge_inputs_[*pending_advance_];
      previous.record = previous.reader->next();
      pending_advance_.reset();
    }
    const auto winner = smallest_input();
    if (!winner.has_value()) {
      close_merge_inputs();
      account_io(before);
      return std::nullopt;
    }

    MergeInput &input = merge_inputs_[*winner];
    Tuple result = switcher::decode(*input.record, schema_);
    pending_advance_ = *winner;
    ++counter_.tuples_out;
    account_io(before);
    return result;
  } catch (...) {
    account_io(before);
    state_ = State::FAILED;
    throw;
  }
}

void SortOperator::close() {
  if (state_ != State::OPEN && state_ != State::FAILED)
    throw std::logic_error("Sort can only be closed when open or failed!");

  state_ = State::FAILED;
  if (input_needs_close_) {
    input_->close();
    input_needs_close_ = false;
  }
  const auto before = buffer_pool_.stats();
  try {
    close_merge_inputs();
    while (!owned_files_.empty())
      remove_temp_file(owned_files_.back());
    run_files_.clear();
  } catch (...) {
    account_io(before);
    throw;
  }
  account_io(before);
  state_ = State::CLOSED;
}

Operator::Counter SortOperator::counters() const { return counter_; }

void SortOperator::merge_runs() {
  const std::size_t fan_in = workspace_frames_ - 1;
  while (run_files_.size() > 1) {
    std::vector<std::size_t> next_runs;
    next_runs.reserve((run_files_.size() - 1) / fan_in + 1);
    std::size_t begin = 0;
    while (begin < run_files_.size()) {
      const std::size_t count = std::min(fan_in, run_files_.size() - begin);
      if (count == 1)
        // pikachu i choose carry forward!
        next_runs.push_back(run_files_[begin]);
      else
        next_runs.push_back(write_merged_run(
            std::span<const std::size_t>(run_files_).subspan(begin, count)));
      begin += count;
    }
    run_files_ = std::move(next_runs);
  }
}

std::size_t SortOperator::write_merged_run(std::span<const std::size_t> files) {
  prepare_merge(files);
  const std::size_t output = create_temp_file();
  storage::TempFile writer(buffer_pool_, output);
  while (auto winner = smallest_input()) {
    MergeInput &input = merge_inputs_[*winner];
    writer.append(*input.record);
    input.record = input.reader->next();
  }
  close_merge_inputs();

  for (std::size_t file : files)
    remove_temp_file(file);
  return output;
}

void SortOperator::prepare_merge(std::span<const std::size_t> files) {
  close_merge_inputs();
  merge_inputs_.reserve(files.size());
  for (std::size_t file : files) {
    merge_inputs_.emplace_back();
    MergeInput &input = merge_inputs_.back();
    input.reader =
        std::make_unique<storage::TempFileReader>(buffer_pool_, file);
    input.record = input.reader->next();
  }
}

std::optional<std::size_t> SortOperator::smallest_input() {
  std::optional<std::size_t> winner;
  for (std::size_t i = 0; i < merge_inputs_.size(); ++i) {
    if (!merge_inputs_[i].record.has_value())
      continue;
    if (!winner.has_value()) {
      winner = i;
      continue;
    }

    ++counter_.comparisons;
    if (switcher::compare_records(*merge_inputs_[i].record,
                                  *merge_inputs_[*winner].record, schema_,
                                  keys_) < 0)
      winner = i;
  }
  return winner;
}

void SortOperator::close_merge_inputs() {
  pending_advance_.reset();
  for (MergeInput &input : merge_inputs_) {
    if (input.reader)
      input.reader->close();
    input.record.reset();
  }
  merge_inputs_.clear();
}

bool SortOperator::try_add_record(Page &page, std::size_t page_id,
                                  std::span<RecordPosition> positions,
                                  std::size_t &record_count, const Tuple &tuple,
                                  std::size_t record_size) {
  if (record_size > page.size() - 4)
    throw std::length_error("Record is too large for a workspace page!");
  if (record_count > positions.size())
    throw std::logic_error("Invalid workspace record count!");
  if (record_count == positions.size())
    return false;

  const std::size_t end = storage::temp_page::used_end(page);
  const std::size_t available = page.size() - end;
  if (available < 2 || record_size > available - 2)
    return false;

  const std::size_t offset = end + 2;
  const std::size_t length = switcher::encode_into(
      tuple, schema_, std::span<char>(page).subspan(offset, record_size));

  storage::help_me_god::write_u16(page, end,
                                  static_cast<std::uint16_t>(length));
  storage::help_me_god::write_u16(page, 0,
                                  static_cast<std::uint16_t>(offset + length));
  positions[record_count] = {page_id, offset, length};
  ++record_count;
  return true;
}

void SortOperator::generate_runs() {
  auto before = buffer_pool_.stats();
  std::size_t workspace_file;
  std::size_t index_file;
  try {
    workspace_file = create_temp_file();
    index_file = create_temp_file();
    buffer_pool_.new_page(index_file);
  } catch (...) {
    account_io(before);
    throw;
  }
  account_io(before);

  std::size_t record_count = 0;
  std::size_t page_id = 0;
  bool initialize_page = true;
  while (auto tuple = input_->next()) {
    const std::size_t record_size = switcher::encoded_size(*tuple, schema_);
    if (record_size > 4092)
      throw std::length_error("Record is too large for a workspace page!");

    before = buffer_pool_.stats();
    try {
      if (record_count == 4096 / 8) {
        write_run(workspace_file, index_file, record_count);
        record_count = 0;
        page_id = 0;
        initialize_page = true;
      }

      for (;;) {
        if (page_id >= buffer_pool_.num_pages(workspace_file))
          buffer_pool_.new_page(workspace_file);
        Page &page = buffer_pool_.fetch_page(workspace_file, page_id);

        RecordPosition position{};
        std::size_t inserted = 0;
        bool appended;
        try {
          if (initialize_page)
            storage::temp_page::initialize(page);
          appended = try_add_record(page, page_id,
                                    std::span<RecordPosition>(&position, 1),
                                    inserted, *tuple, record_size);
        } catch (...) {
          buffer_pool_.unpin_page(workspace_file, page_id, initialize_page);
          throw;
        }
        buffer_pool_.unpin_page(workspace_file, page_id,
                                initialize_page || appended);

        if (appended) {
          Page &index = buffer_pool_.fetch_page(index_file, 0);
          try {
            write_position(index, record_count, position);
          } catch (...) {
            buffer_pool_.unpin_page(index_file, 0, true);
            throw;
          }
          buffer_pool_.unpin_page(index_file, 0, true);
          ++record_count;
          initialize_page = false;
          break;
        }

        if (page_id + 1 < workspace_frames_ - 1) {
          ++page_id;
          initialize_page = true;
          continue;
        }
        if (record_count == 0)
          throw std::length_error(
              "Sort cannot fit one record in its workspace!");

        write_run(workspace_file, index_file, record_count);
        record_count = 0;
        page_id = 0;
        initialize_page = true;
        // Retry this same boy
      }
    } catch (...) {
      account_io(before);
      throw;
    }
    account_io(before);
  }

  before = buffer_pool_.stats();
  try {
    if (record_count != 0)
      write_run(workspace_file, index_file, record_count);
    remove_temp_file(workspace_file);
    remove_temp_file(index_file);
  } catch (...) {
    account_io(before);
    throw;
  }
  account_io(before);
}

SortOperator::RecordPosition
SortOperator::read_position(const Page &index, std::size_t position) const {
  if (position >= index.size() / 8)
    throw std::out_of_range("Sort position index is out of range!");

  const std::size_t offset = position * 8;
  return {storage::help_me_god::read_u32(index, offset),
          storage::help_me_god::read_u16(index, offset + 4),
          storage::help_me_god::read_u16(index, offset + 6)};
}

void SortOperator::write_position(Page &index, std::size_t position,
                                  const RecordPosition &record) const {
  if (position >= index.size() / 8)
    throw std::out_of_range("Sort position index is out of range!");
  if (record.page_id > std::numeric_limits<std::uint32_t>::max() ||
      record.offset > 4096 || record.length > 4096 - record.offset)
    throw std::out_of_range("Invalid sort record position!");

  // uint32 page ID then a uint16 byte offset guy then a uint16 byte length
  const std::size_t offset = position * 8;
  storage::help_me_god::write_u32(index, offset,
                                  static_cast<std::uint32_t>(record.page_id));
  storage::help_me_god::write_u16(index, offset + 4,
                                  static_cast<std::uint16_t>(record.offset));
  storage::help_me_god::write_u16(index, offset + 6,
                                  static_cast<std::uint16_t>(record.length));
}

SortOperator::RecordPosition SortOperator::read_position(std::size_t index_file,
                                                         std::size_t position) {
  Page &index = buffer_pool_.fetch_page(index_file, 0);
  RecordPosition result;
  try {
    result = read_position(index, position);
  } catch (...) {
    buffer_pool_.unpin_page(index_file, 0, false);
    throw;
  }
  buffer_pool_.unpin_page(index_file, 0, false);
  return result;
}

void SortOperator::write_position(std::size_t index_file, std::size_t position,
                                  const RecordPosition &record) {
  Page &index = buffer_pool_.fetch_page(index_file, 0);
  try {
    write_position(index, position, record);
  } catch (...) {
    buffer_pool_.unpin_page(index_file, 0, true);
    throw;
  }
  buffer_pool_.unpin_page(index_file, 0, true);
}

std::span<const char>
SortOperator::record_bytes(const Page &page,
                           const RecordPosition &record) const {
  if (record.offset > page.size() ||
      record.length > page.size() - record.offset)
    throw std::out_of_range("Invalid sort record position!");
  return std::span<const char>(page).subspan(record.offset, record.length);
}

int SortOperator::compare_positions(std::size_t workspace_file,
                                    const RecordPosition &left,
                                    const RecordPosition &right) {
  ++counter_.comparisons;
  Page &left_page = buffer_pool_.fetch_page(workspace_file, left.page_id);
  int result;
  try {
    if (left.page_id == right.page_id)
      result = switcher::compare_records(record_bytes(left_page, left),
                                         record_bytes(left_page, right),
                                         schema_, keys_);
    else {
      Page &right_page = buffer_pool_.fetch_page(workspace_file, right.page_id);
      try {
        result = switcher::compare_records(record_bytes(left_page, left),
                                           record_bytes(right_page, right),
                                           schema_, keys_);
      } catch (...) {
        buffer_pool_.unpin_page(workspace_file, right.page_id, false);
        throw;
      }
      buffer_pool_.unpin_page(workspace_file, right.page_id, false);
    }
  } catch (...) {
    buffer_pool_.unpin_page(workspace_file, left.page_id, false);
    throw;
  }
  buffer_pool_.unpin_page(workspace_file, left.page_id, false);
  return result;
}

void SortOperator::write_run(std::size_t workspace_file, std::size_t index_file,
                             std::size_t record_count) {
  // insertion sort gg ez
  for (std::size_t i = 1; i < record_count; ++i) {
    const RecordPosition current = read_position(index_file, i);
    std::size_t position = i;
    while (position != 0) {
      const RecordPosition previous = read_position(index_file, position - 1);
      if (compare_positions(workspace_file, previous, current) <= 0)
        break;
      write_position(index_file, position, previous);
      --position;
    }
    write_position(index_file, position, current);
  }

  const std::size_t run_file = create_temp_file();
  storage::TempFile writer(buffer_pool_, run_file);
  for (std::size_t i = 0; i < record_count; ++i) {
    const RecordPosition record = read_position(index_file, i);

    Page &data = buffer_pool_.fetch_page(workspace_file, record.page_id);
    try {
      writer.append(record_bytes(data, record));
    } catch (...) {
      buffer_pool_.unpin_page(workspace_file, record.page_id, false);
      throw;
    }
    buffer_pool_.unpin_page(workspace_file, record.page_id, false);
  }
  run_files_.push_back(run_file);
}

std::size_t SortOperator::create_temp_file() {
  owned_files_.reserve(owned_files_.size() + 1);
  std::string path =
      (std::filesystem::temp_directory_path() / "vql-sort-XXXXXX").string();
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

void SortOperator::remove_temp_file(std::size_t file_id) {
  buffer_pool_.remove_file(file_id);
  std::erase(owned_files_, file_id);
}

void SortOperator::account_io(storage::BufferPool::Stats before) {
  const auto after = buffer_pool_.stats();
  counter_.page_reads += after.reads - before.reads;
  counter_.page_writes += after.writes - before.writes;
}

} // namespace vql::algebra
