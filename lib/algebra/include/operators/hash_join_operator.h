#ifndef HASH_JOIN_OPERATOR_H
#define HASH_JOIN_OPERATOR_H

#include "buffer_pool.h"
#include "operator.h"
#include "operators/join_operator.h"
#include "temp_file.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace vql::algebra {
class HashJoinOperator : public Operator {
public:
  struct Key {
    std::size_t left;
    std::size_t right;
  };

  HashJoinOperator(std::unique_ptr<Operator> left,
                   std::unique_ptr<Operator> right, std::vector<Key> keys,
                   storage::BufferPool &buffer_pool,
                   std::size_t workspace_frames);
  ~HashJoinOperator() override;

  const Schema &schema() const override;
  void open() override;
  void close() override;
  // Output remains valid until next next()/close(); joined records exceeding
  // one 4096-byte output page cause an error.
  std::optional<Tuple> next() override;
  Counter counters() const override;

private:
  struct Partition {
    std::optional<std::size_t> left;
    std::optional<std::size_t> right;
  };
  struct Entry {
    std::uint64_t next;
    std::span<const char> record;
  };

  void partition_input(bool left_side);
  void append_partition(const Tuple &tuple, bool left_side);
  std::uint64_t hash_key(const Tuple &tuple, bool left_side,
                         std::uint64_t seed) const;
  bool prepare_partition();
  bool build_table(std::size_t file_id);
  bool insert_record(std::span<const char> record);
  Entry entry_at(std::uint64_t address) const;
  bool matching_keys(std::span<const char> left,
                      std::span<const char> right);
  bool fallback_condition(const Tuple &candidate);
  Tuple emit_match(std::span<const char> left,
                    std::span<const char> right);
  void start_fallback(Partition &partition);
  void cleanup_partition();
  void release_table();
  void create_output_page();
  void release_output_page();
  std::size_t create_temp_file();
  void remove_file(std::optional<std::size_t> &file_id);
  void account_io(storage::BufferPool::Stats before);

  std::unique_ptr<Operator> left_;
  std::unique_ptr<Operator> right_;
  std::vector<Key> keys_;
  storage::BufferPool &buffer_pool_;
  std::size_t workspace_frames_;
  std::vector<Partition> partitions_;
  std::size_t next_partition_{0};
  std::optional<std::size_t> current_partition_;
  bool left_needs_close_{false};
  bool right_needs_close_{false};
  std::optional<std::size_t> encoding_file_id_;
  // B-2 build frames: one bucket-head page, then up to B-3 row/link pages.
  // The probe reader and output each use one more frame. Table metadata lives
  // inside these pages; overflow (including metadata overhead) falls back to BNL.
  // A three-frame share leaves no row page, so every nonempty pair uses BNL.
  std::optional<std::size_t> table_file_id_;
  std::vector<Page *> table_pages_;
  std::unique_ptr<storage::TempFileReader> probe_reader_;
  std::optional<std::span<const char>> probe_record_;
  std::uint64_t chain_{0};
  std::optional<std::size_t> output_file_id_;
  Page *output_page_{nullptr};
  bool output_dirty_{false};
  std::unique_ptr<JoinOperator> fallback_;
  bool exhausted_{false};
};
} // namespace vql::algebra

#endif // HASH_JOIN_OPERATOR_H
