#ifndef SORT_DISTINCT_OPERATOR_H
#define SORT_DISTINCT_OPERATOR_H

#include "buffer_pool.h"
#include "operator.h"
#include "operators/sort_operator.h"

#include <cstddef>
#include <memory>
#include <optional>

namespace vql::algebra {
class SortDistinctOperator : public Operator {
public:
  SortDistinctOperator(std::unique_ptr<Operator> input,
                       storage::BufferPool &buffer_pool,
                       std::size_t workspace_frames);

  const Schema &schema() const override;
  void open() override;
  void close() override;
  std::optional<Tuple> next() override;
  Counter counters() const override;

private:
  void create_previous_file();
  void account_io(storage::BufferPool::Stats before);

  std::unique_ptr<SortOperator> sorter_;
  storage::BufferPool &buffer_pool_;
  std::optional<std::size_t> previous_file_id_;
  std::optional<std::size_t> previous_size_;
  bool sorter_needs_close_{false};
  bool sleepy_{false};
};
} // namespace vql::algebra

#endif // SORT_DISTINCT_OPERATOR_H
