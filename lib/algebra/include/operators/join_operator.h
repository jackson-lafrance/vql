#ifndef JOIN_OPERATOR_H
#define JOIN_OPERATOR_H

#include "buffer_pool.h"
#include "operator.h"
#include "operators/join_inputs.h"
#include "temp_file.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace vql::algebra {
class JoinOperator : public Operator {
public:
  JoinOperator(std::unique_ptr<Operator> left,
               std::unique_ptr<Operator> right,
               std::function<bool(const Tuple &)> condition,
               storage::BufferPool &buffer_pool,
               std::size_t workspace_frames);
  ~JoinOperator() override;

  const Schema &schema() const override;
  void open() override;
  void close() override;
  std::optional<Tuple> next() override;
  Counter counters() const override;

private:
  struct OuterPage {
    std::size_t page_id;
    Page *page;
  };

  bool load_outer_block();
  void pin_outer_block();
  void unpin_outer_block();
  void release_outer_block();
  std::optional<std::span<const char>> next_outer_record();
  Tuple make_candidate(std::span<const char> left,
                       std::span<const char> right);
  void create_output_page();
  void release_output_page();
  void account_io(storage::BufferPool::Stats before);

  detail::JoinInputs inputs_;
  storage::BufferPool &buffer_pool_;
  std::function<bool(const Tuple &)> condition_;
  std::size_t workspace_frames_;
  std::size_t left_file_id_{0};
  std::size_t right_file_id_{0};
  std::size_t left_page_count_{0};
  std::size_t next_left_page_{0};
  std::vector<OuterPage> outer_pages_;
  std::size_t outer_page_index_{0};
  std::size_t outer_position_{2};
  std::unique_ptr<storage::TempFileReader> right_reader_;
  bool has_right_record_{false};
  std::optional<std::size_t> output_file_id_;
  Page *output_page_{nullptr};
  bool output_dirty_{false};
  bool exhausted_{false};
};
} // namespace vql::algebra

#endif // JOIN_OPERATOR_H
