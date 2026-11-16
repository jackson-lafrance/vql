#ifndef DISTINCT_OPERATOR_H
#define DISTINCT_OPERATOR_H

#include "buffer_pool.h"
#include "operator.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace vql::algebra {
class HashDistinctOperator : public Operator {
public:
  HashDistinctOperator(std::unique_ptr<Operator> input,
                       storage::BufferPool &buffer_pool,
                       std::size_t partition_count);

  const Schema &schema() const override;
  void open() override;
  void close() override;
  std::optional<Tuple> next() override;
  Counter counters() const override;

private:
  void partition_input();
  bool prepare_next_batch();
  bool batch_contains(std::span<const char> record);
  std::size_t create_temp_file();
  void remove_temp_file(std::size_t file_id);

  std::unique_ptr<Operator> input_;
  storage::BufferPool &buffer_pool_;
  std::size_t partition_count_;

  std::vector<std::size_t> pending_files_;
  std::vector<std::size_t> owned_files_;

  std::optional<std::size_t> batch_file_id_;
  Page *batch_page_{nullptr};
  std::size_t output_position{2};

  bool input_needs_close_{false};
  bool sleepy_{false};
};
} // namespace vql::algebra

#endif // DISTINCT_OPERATOR_H
