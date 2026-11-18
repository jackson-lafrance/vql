#ifndef EXCEPT_OPERATOR_H
#define EXCEPT_OPERATOR_H

#include "buffer_pool.h"
#include "operator.h"
#include "operators/sort_operator.h"

#include <cstddef>
#include <memory>
#include <optional>

namespace vql::algebra {
class ExceptOperator : public Operator {
public:
  ExceptOperator(std::unique_ptr<Operator> left_input,
                 std::unique_ptr<Operator> right_input,
                 storage::BufferPool &buffer_pool,
                 std::size_t workspace_frames);

  const Schema &schema() const override;
  void open() override;
  void close() override;
  std::optional<Tuple> next() override;
  Counter counters() const override;

private:
  int compare(const Tuple &left, const Tuple &right);
  int compare_to_previous(const Tuple &tuple);
  void remember(const Tuple &tuple);
  void create_previous_page();
  void account_io(storage::BufferPool::Stats before);

  storage::BufferPool &buffer_pool_;
  std::unique_ptr<SortOperator> left_;
  std::unique_ptr<SortOperator> right_;
  std::optional<Tuple> left_tuple_;
  std::optional<Tuple> right_tuple_;
  bool left_needs_close_{false};
  bool right_needs_close_{false};
  std::optional<std::size_t> previous_file_id_;
  Page *previous_page_{nullptr};
  std::optional<std::size_t> previous_size_;
};
} // namespace vql::algebra

#endif // EXCEPT_OPERATOR_H
