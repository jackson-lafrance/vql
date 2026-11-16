#ifndef SEQUENTIAL_OPERATOR_H
#define SEQUENTIAL_OPERATOR_H

#include "buffer_pool.h"
#include "heap_scan.h"
#include "operator.h"

#include <memory>

namespace vql::algebra {
class SequentialOperator : public Operator {
public:
  SequentialOperator(storage::BufferPool &buffer_pool, std::size_t file_id,
                     Schema schema);

  const Schema &schema() const override;
  void open() override;
  void close() override;
  std::optional<Tuple> next() override;
  Counter counters() const override;

private:
  storage::BufferPool &buffer_pool_;
  std::size_t file_id_;
  std::unique_ptr<storage::HeapScan> heap_scan_;
};
} // namespace vql::algebra

#endif // SEQUENTIAL_OPERATOR_H
