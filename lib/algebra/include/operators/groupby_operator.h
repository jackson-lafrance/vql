#ifndef GROUPBY_OPERATOR_H
#define GROUPBY_OPERATOR_H

#include "buffer_pool.h"
#include "operator.h"
#include <memory>

namespace vql::algebra {
class GroupbyOperator : public Operator {
public:
  enum class Mode { HASH, SORT };

  struct Options {
    std::size_t partition_count{0};
    std::size_t workspace_frames{0};
  };

  GroupbyOperator(std::unique_ptr<Operator> input,
                  storage::BufferPool &buffer_pool, Mode mode, Options options);

  const Schema &schema() const override;
  void open() override;
  void close() override;
  std::optional<Tuple> next() override;
  Counter counters() const override;

private:
  std::unique_ptr<Operator> dracula_imp_;
};
} // namespace vql::algebra

#endif // GROUPBY_OPERATOR_H
