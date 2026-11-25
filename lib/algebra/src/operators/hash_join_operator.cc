#include "operators/hash_join_operator.h"

#include "help_me_god.h"
#include "switcher.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace vql::algebra {
namespace {
constexpr std::size_t PAGE_SIZE = 4096;
constexpr std::size_t LINK_SIZE = 8;
constexpr std::size_t ENTRY_HEADER = LINK_SIZE + 2;
constexpr std::size_t BUCKETS = PAGE_SIZE / LINK_SIZE;
constexpr std::uint64_t PARTITION_SEED = 14695981039346656037ULL;
constexpr std::uint64_t TABLE_SEED = 7809847782465536322ULL;

std::uint64_t read_link(const Page &page, std::size_t offset) {
  return storage::help_me_god::read_u32(page, offset) |
         (static_cast<std::uint64_t>(
              storage::help_me_god::read_u32(page, offset + 4))
          << 32);
}

void write_link(Page &page, std::size_t offset, std::uint64_t value) {
  storage::help_me_god::write_u32(page, offset,
                                  static_cast<std::uint32_t>(value));
  storage::help_me_god::write_u32(page, offset + 4,
                                  static_cast<std::uint32_t>(value >> 32));
}

// Borrows a partition file owned by HashJoinOperator. Used only as a BNL child;
// BNL's input-saving helper closes it before allocating its join workspace.
class PartitionSource : public Operator {
public:
  PartitionSource(storage::BufferPool &pool, std::size_t file_id, Schema schema)
      : pool_(pool), file_id_(file_id) {
    schema_ = std::move(schema);
  }
  const Schema &schema() const override { return schema_; }
  void open() override {
    if (state_ != State::CREATED)
      throw std::logic_error("Hash partition can only be opened once!");
    state_ = State::FAILED;
    reader_ = std::make_unique<storage::TempFileReader>(pool_, file_id_);
    state_ = State::OPEN;
  }
  std::optional<Tuple> next() override {
    if (state_ != State::OPEN)
      throw std::logic_error("Hash partition can only be queried when open!");
    const auto before = pool_.stats();
    try {
      const auto record = reader_->next();
      std::optional<Tuple> result;
      if (record) {
        result.emplace(schema_, *record);
        ++counter_.tuples_out;
      }
      const auto after = pool_.stats();
      counter_.page_reads += after.reads - before.reads;
      counter_.page_writes += after.writes - before.writes;
      return result;
    } catch (...) {
      const auto after = pool_.stats();
      counter_.page_reads += after.reads - before.reads;
      counter_.page_writes += after.writes - before.writes;
      state_ = State::FAILED;
      throw;
    }
  }
  void close() override {
    if (state_ != State::OPEN && state_ != State::FAILED)
      throw std::logic_error("Hash partition must be open or failed to close!");
    state_ = State::FAILED;
    if (reader_)
      reader_->close();
    state_ = State::CLOSED;
  }
  Counter counters() const override { return counter_; }

private:
  storage::BufferPool &pool_;
  std::size_t file_id_;
  std::unique_ptr<storage::TempFileReader> reader_;
};
} // namespace

HashJoinOperator::HashJoinOperator(std::unique_ptr<Operator> left,
                                   std::unique_ptr<Operator> right,
                                   std::vector<Key> keys,
                                   storage::BufferPool &buffer_pool,
                                   std::size_t workspace_frames)
    : left_(std::move(left)), right_(std::move(right)), keys_(std::move(keys)),
      buffer_pool_(buffer_pool), workspace_frames_(workspace_frames) {
  if (!left_ || !right_)
    throw std::invalid_argument("HashJoin requires two input operators!");
  if (keys_.empty())
    throw std::invalid_argument("HashJoin requires an equality key!");
  if (workspace_frames_ < 3)
    throw std::invalid_argument("HashJoin requires at least three workspace frames!");
  if (workspace_frames_ - 2 >
      std::numeric_limits<std::uint64_t>::max() / PAGE_SIZE)
    throw std::invalid_argument("HashJoin workspace is too large to address!");
  for (const Key &key : keys_)
    if (key.left >= left_->schema().size() || key.right >= right_->schema().size())
      throw std::out_of_range("HashJoin key position is out of range!");
  schema_ = concat_schemas(left_->schema(), right_->schema());
}

HashJoinOperator::~HashJoinOperator() {
  if (state_ == State::OPEN || state_ == State::FAILED) {
    try {
      close();
    } catch (...) {
      // Explicit close reports errors and can be retried; destruction cannot throw.
    }
  }
}

const Schema &HashJoinOperator::schema() const { return schema_; }

void HashJoinOperator::open() {
  if (state_ != State::CREATED)
    throw std::logic_error("HashJoin can only be opened once!");
  state_ = State::FAILED;
  for (const Key &key : keys_)
    if (left_->schema()[key.left].type.data_type() !=
        right_->schema()[key.right].type.data_type())
      throw std::invalid_argument(
          "HashJoin equality keys must have the same data type!");
  for (std::size_t i = 0; i < schema_.size(); ++i)
    for (std::size_t j = 0; j < i; ++j)
      if (schema_[i].name == schema_[j].name)
        throw std::invalid_argument("HashJoin schema contains duplicate names!");

  partitions_.resize(workspace_frames_ - 1);
  for (Partition &partition : partitions_) {
    partition.left = create_temp_file();
    partition.right = create_temp_file();
  }
  partition_input(true);
  partition_input(false);

  const auto before = buffer_pool_.stats();
  try {
    exhausted_ = !prepare_partition();
  } catch (...) {
    account_io(before);
    throw;
  }
  account_io(before);
  state_ = State::OPEN;
}

void HashJoinOperator::partition_input(bool left_side) {
  Operator &input = left_side ? *left_ : *right_;
  bool &needs_close = left_side ? left_needs_close_ : right_needs_close_;
  needs_close = true;
  input.open();
  while (const auto tuple = input.next())
    append_partition(*tuple, left_side);
  input.close();
  needs_close = false;

  const auto before = buffer_pool_.stats();
  try {
    remove_file(encoding_file_id_);
  } catch (...) {
    account_io(before);
    throw;
  }
  account_io(before);
}

void HashJoinOperator::append_partition(const Tuple &tuple, bool left_side) {
  const auto before = buffer_pool_.stats();
  try {
    const Schema &schema = left_side ? left_->schema() : right_->schema();
    const std::size_t size = switcher::encoded_size(tuple, schema);
    if (size > PAGE_SIZE - 4)
      throw std::length_error("HashJoin input exceeds a temporary record page!");
    Partition &partition =
        partitions_[hash_key(tuple, left_side, PARTITION_SEED) % partitions_.size()];
    storage::TempFile writer(buffer_pool_,
                              *(left_side ? partition.left : partition.right));
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

std::uint64_t HashJoinOperator::hash_key(const Tuple &tuple, bool left_side,
                                        std::uint64_t seed) const {
  // Fixed FNV-1a seeds make partitioning repeatable. Type tags and string lengths
  // separate composite-key fields; different seeds are used for partition/build.
  std::uint64_t hash = seed;
  const auto mix = [&](std::uint8_t byte) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  };
  for (const Key &key : keys_) {
    const Value value = tuple[left_side ? key.left : key.right];
    if (const auto *number = std::get_if<std::int32_t>(&value)) {
      mix(1);
      const auto bits = static_cast<std::uint32_t>(*number);
      for (unsigned shift = 0; shift < 32; shift += 8)
        mix(static_cast<std::uint8_t>(bits >> shift));
    } else {
      mix(2);
      const auto text = std::get<std::string_view>(value);
      const auto length = static_cast<std::uint64_t>(text.size());
      for (unsigned shift = 0; shift < 64; shift += 8)
        mix(static_cast<std::uint8_t>(length >> shift));
      for (char byte : text)
        mix(static_cast<std::uint8_t>(static_cast<unsigned char>(byte)));
    }
  }
  return hash;
}

bool HashJoinOperator::prepare_partition() {
  while (next_partition_ < partitions_.size()) {
    current_partition_ = next_partition_++;
    Partition &partition = partitions_[*current_partition_];
    const std::size_t left_pages = buffer_pool_.num_pages(*partition.left);
    if (left_pages == 0 || buffer_pool_.num_pages(*partition.right) == 0) {
      cleanup_partition();
      continue;
    }
    // The stated B-1 overflow threshold is mandatory; actual row/link/index
    // storage may overflow sooner because the probe and output need frames too.
    if (left_pages > workspace_frames_ - 1 || !build_table(*partition.left)) {
      release_table();
      start_fallback(partition);
    } else {
      create_output_page();
      probe_reader_ =
          std::make_unique<storage::TempFileReader>(buffer_pool_, *partition.right);
    }
    return true;
  }
  return false;
}

bool HashJoinOperator::build_table(std::size_t file_id) {
  table_pages_.reserve(workspace_frames_ - 2);
  table_file_id_ = create_temp_file();
  const std::size_t index = buffer_pool_.new_page(*table_file_id_);
  Page &heads = buffer_pool_.fetch_page(*table_file_id_, index);
  table_pages_.push_back(&heads);
  table_dirty_ = true;
  heads.fill(0);

  storage::TempFileReader reader(buffer_pool_, file_id);
  while (const auto record = reader.next())
    if (!insert_record(*record))
      return false;
  return true;
}

bool HashJoinOperator::insert_record(std::span<const char> record) {
  if (record.size() > PAGE_SIZE - 2 - ENTRY_HEADER)
    return false;
  if (table_pages_.size() == 1 ||
      PAGE_SIZE - storage::temp_page::used_end(*table_pages_.back()) <
          ENTRY_HEADER + record.size()) {
    if (table_pages_.size() == workspace_frames_ - 2)
      return false;
    const std::size_t id = buffer_pool_.new_page(*table_file_id_);
    Page &page = buffer_pool_.fetch_page(*table_file_id_, id);
    table_pages_.push_back(&page);
    storage::temp_page::initialize(page);
  }

  const Tuple tuple(left_->schema(), record);
  const auto bucket = hash_key(tuple, true, TABLE_SEED) % BUCKETS;
  Page &page = *table_pages_.back();
  const std::size_t offset = storage::temp_page::used_end(page);
  const auto address =
      static_cast<std::uint64_t>(table_pages_.size() - 1) * PAGE_SIZE + offset;
  write_link(page, offset, read_link(*table_pages_[0], bucket * LINK_SIZE));
  storage::help_me_god::write_u16(page, offset + LINK_SIZE,
                                  static_cast<std::uint16_t>(record.size()));
  std::copy(record.begin(), record.end(), page.begin() + offset + ENTRY_HEADER);
  storage::help_me_god::write_u16(
      page, 0, static_cast<std::uint16_t>(offset + ENTRY_HEADER + record.size()));
  write_link(*table_pages_[0], bucket * LINK_SIZE, address);
  return true;
}

HashJoinOperator::Entry HashJoinOperator::entry_at(std::uint64_t address) const {
  const auto page_id = address / PAGE_SIZE;
  const auto offset = static_cast<std::size_t>(address % PAGE_SIZE);
  if (page_id == 0 || page_id >= table_pages_.size())
    throw std::logic_error("Invalid HashJoin table address!");
  const Page &page = *table_pages_[static_cast<std::size_t>(page_id)];
  const std::size_t end = storage::temp_page::used_end(page);
  if (offset < 2 || offset > end || ENTRY_HEADER > end - offset)
    throw std::logic_error("Invalid HashJoin entry header!");
  const auto length = storage::help_me_god::read_u16(page, offset + LINK_SIZE);
  if (length > end - offset - ENTRY_HEADER)
    throw std::logic_error("Invalid HashJoin entry length!");
  return {read_link(page, offset),
          std::span<const char>(page).subspan(offset + ENTRY_HEADER, length)};
}

std::optional<Tuple> HashJoinOperator::next() {
  if (state_ != State::OPEN)
    throw std::logic_error("HashJoin can only be queried when open!");
  if (exhausted_)
    return std::nullopt;

  const auto before = buffer_pool_.stats();
  try {
    for (;;) {
      if (!current_partition_ && !prepare_partition()) {
        exhausted_ = true;
        account_io(before);
        return std::nullopt;
      }
      if (fallback_) {
        if (auto tuple = fallback_->next()) {
          Tuple result = tuple->with_schema(schema_);
          ++counter_.tuples_out;
          account_io(before);
          return result;
        }
        cleanup_partition();
        continue;
      }
      pin_table();
      std::optional<std::span<const char>> probe_record;
      if (has_probe_record_) {
        probe_record = probe_reader_->current();
      } else {
        probe_record = probe_reader_->next();
        if (!probe_record) {
          cleanup_partition();
          continue;
        }
        has_probe_record_ = true;
        const Tuple probe(right_->schema(), *probe_record);
        const auto bucket = hash_key(probe, false, TABLE_SEED) % BUCKETS;
        chain_ = read_link(*table_pages_[0], bucket * LINK_SIZE);
      }
      while (chain_ != 0) {
        const Entry entry = entry_at(chain_);
        chain_ = entry.next;
        if (matching_keys(entry.record, *probe_record)) {
          Tuple result = emit_match(entry.record, *probe_record);
          probe_reader_->release_page();
          unpin_table();
          ++counter_.tuples_out;
          account_io(before);
          return result;
        }
      }
      has_probe_record_ = false;
    }
  } catch (...) {
    state_ = State::FAILED;
    account_io(before);
    throw;
  }
}

bool HashJoinOperator::matching_keys(std::span<const char> left,
                                      std::span<const char> right) {
  ++counter_.comparisons;
  const Tuple build(left_->schema(), left);
  const Tuple probe(right_->schema(), right);
  for (const Key &key : keys_)
    if (compare_values(build[key.left], probe[key.right]) != 0)
      return false;
  return true;
}

bool HashJoinOperator::fallback_condition(const Tuple &candidate) {
  ++counter_.comparisons;
  const auto right_start = left_->schema().size();
  for (const Key &key : keys_)
    if (compare_values(candidate[key.left], candidate[right_start + key.right]) != 0)
      return false;
  return true;
}

Tuple HashJoinOperator::emit_match(std::span<const char> left,
                                    std::span<const char> right) {
  if (left.size() > output_page_->size() ||
      right.size() > output_page_->size() - left.size())
    throw std::length_error("HashJoin match exceeds its 4096-byte output page!");
  std::copy(left.begin(), left.end(), output_page_->begin());
  std::copy(right.begin(), right.end(), output_page_->begin() + left.size());
  output_dirty_ = true;
  return Tuple(schema_, std::span<const char>(*output_page_)
                            .first(left.size() + right.size()));
}

void HashJoinOperator::start_fallback(Partition &partition) {
  fallback_ = std::make_unique<JoinOperator>(
      std::make_unique<PartitionSource>(buffer_pool_, *partition.left,
                                         left_->schema()),
      std::make_unique<PartitionSource>(buffer_pool_, *partition.right,
                                         right_->schema()),
      [this](const Tuple &tuple) { return fallback_condition(tuple); },
      buffer_pool_, workspace_frames_);
  fallback_->open();
}

void HashJoinOperator::cleanup_partition() {
  std::exception_ptr failure;
  if (fallback_) {
    try {
      fallback_->close();
      fallback_.reset();
    } catch (...) {
      failure = std::current_exception();
    }
  }
  has_probe_record_ = false;
  chain_ = 0;
  if (probe_reader_) {
    try {
      probe_reader_->close();
      probe_reader_.reset();
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
  try {
    release_table();
  } catch (...) {
    if (!failure)
      failure = std::current_exception();
  }
  if (current_partition_) {
    Partition &partition = partitions_[*current_partition_];
    for (auto *file : {&partition.left, &partition.right}) {
      try {
        remove_file(*file);
      } catch (...) {
        if (!failure)
          failure = std::current_exception();
      }
    }
  }
  if (failure)
    std::rethrow_exception(failure);
  current_partition_.reset();
}

void HashJoinOperator::pin_table() {
  for (std::size_t id = 0; id < table_pages_.size(); ++id)
    if (!table_pages_[id])
      table_pages_[id] = &buffer_pool_.fetch_page(*table_file_id_, id);
}

void HashJoinOperator::unpin_table() {
  for (std::size_t id = 0; id < table_pages_.size(); ++id) {
    if (table_pages_[id]) {
      buffer_pool_.unpin_page(*table_file_id_, id, table_dirty_);
      table_pages_[id] = nullptr;
    }
  }
  table_dirty_ = false;
}

void HashJoinOperator::release_table() {
  unpin_table();
  table_pages_.clear();
  remove_file(table_file_id_);
}

void HashJoinOperator::close() {
  if (state_ != State::OPEN && state_ != State::FAILED)
    throw std::logic_error("HashJoin must be OPEN or FAILED before close!");
  state_ = State::FAILED;
  exhausted_ = true;
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
    cleanup_partition();
  } catch (...) {
    if (!failure)
      failure = std::current_exception();
  }
  try {
    remove_file(encoding_file_id_);
  } catch (...) {
    if (!failure)
      failure = std::current_exception();
  }
  for (Partition &partition : partitions_)
    for (auto *file : {&partition.left, &partition.right}) {
      try {
        remove_file(*file);
      } catch (...) {
        if (!failure)
          failure = std::current_exception();
      }
    }
  account_io(before);
  if (failure)
    std::rethrow_exception(failure);
  state_ = State::CLOSED;
}

Operator::Counter HashJoinOperator::counters() const { return counter_; }

void HashJoinOperator::create_output_page() {
  output_file_id_ = create_temp_file();
  buffer_pool_.new_page(*output_file_id_);
  output_page_ = &buffer_pool_.fetch_page(*output_file_id_, 0);
  output_dirty_ = false;
}

void HashJoinOperator::release_output_page() {
  if (output_page_) {
    buffer_pool_.unpin_page(*output_file_id_, 0, output_dirty_);
    output_page_ = nullptr;
  }
  remove_file(output_file_id_);
}

std::size_t HashJoinOperator::create_temp_file() {
  std::string path =
      (std::filesystem::temp_directory_path() / "vql-hash-join-XXXXXX").string();
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

void HashJoinOperator::remove_file(std::optional<std::size_t> &file_id) {
  if (!file_id)
    return;
  buffer_pool_.remove_file(*file_id);
  file_id.reset();
}

void HashJoinOperator::account_io(storage::BufferPool::Stats before) {
  const auto after = buffer_pool_.stats();
  counter_.page_reads += after.reads - before.reads;
  counter_.page_writes += after.writes - before.writes;
}
} // namespace vql::algebra
