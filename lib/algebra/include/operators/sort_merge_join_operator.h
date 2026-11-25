#ifndef SORT_MERGE_JOIN_OPERATOR_H
#define SORT_MERGE_JOIN_OPERATOR_H

#include "buffer_pool.h"
#include "operator.h"
#include "operators/join_inputs.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace vql::algebra {
class SortMergeJoinOperator : public Operator {
public:
  struct Key {
    std::size_t left;
    std::size_t right;
  };

  SortMergeJoinOperator(std::unique_ptr<Operator> left,
                        std::unique_ptr<Operator> right,
                        std::vector<Key> keys,
                        storage::BufferPool &buffer_pool,
                        std::size_t workspace_frames);
  ~SortMergeJoinOperator() override;

  const Schema &schema() const override;
  void open() override;
  void close() override;
  std::optional<Tuple> next() override;
  Counter counters() const override;

private:
  struct Position {
    std::size_t page_id{0};
    std::size_t offset{2};
  };
  struct Cursor {
    std::size_t file_id{0};
    std::size_t page_count{0};
    Position next;
    std::optional<Position> current;
    Page *page{nullptr};
    std::optional<std::span<const char>> record;
  };

  void advance(Cursor &cursor);
  void rewind_right(Position position);
  void unpin_cursor(Cursor &cursor);
  void restore_cursor(Cursor &cursor);
  void close_cursor(Cursor &cursor);
  int compare_keys();
  int compare_left_to_output();
  Tuple emit_match();
  void create_output_page();
  void release_output_page();
  void account_io(storage::BufferPool::Stats before);

  storage::BufferPool &buffer_pool_;
  std::vector<Key> keys_;
  std::unique_ptr<detail::JoinInputs> inputs_;
  Cursor left_;
  Cursor right_;
  std::optional<Position> group_start_;
  bool advance_right_{false};
  std::optional<std::size_t> output_file_id_;
  Page *output_page_{nullptr};
  std::size_t output_size_{0};
  bool output_dirty_{false};
  bool exhausted_{false};
};
} // namespace vql::algebra

#endif // SORT_MERGE_JOIN_OPERATOR_H
