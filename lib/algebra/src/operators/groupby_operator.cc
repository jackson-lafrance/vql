#include "operators/groupby_operator.h"
#include "operators/hash_groupby_operator.h"
#include "operators/sort_groupby_operator.h"

#include <stdexcept>
#include <utility>

namespace vql::algebra {
GroupbyOperator::GroupbyOperator(std::unique_ptr<Operator> input,
                                 storage::BufferPool &buffer_pool, Mode mode,
                                 Options options,
                                 std::vector<std::string> key_names,
                                 std::vector<Aggregate> aggregates) {
  if (!input)
    throw std::invalid_argument("Groupby is missing an input operator!");

  switch (mode) {
  case Mode::SORT:
    dracula_imp_ = std::make_unique<SortGroupbyOperator>(
        std::move(input), buffer_pool, options.workspace_frames,
        std::move(key_names), std::move(aggregates));
    break;
  case Mode::HASH:
    dracula_imp_ = std::make_unique<HashGroupbyOperator>(
        std::move(input), buffer_pool, options.partition_count,
        std::move(key_names), std::move(aggregates));
    break;
  default:
    throw std::invalid_argument("Invalid GROUP BY mode!");
  }
}

const Schema &GroupbyOperator::schema() const { return dracula_imp_->schema(); }

void GroupbyOperator::open() { dracula_imp_->open(); }

void GroupbyOperator::close() { dracula_imp_->close(); }

std::optional<Tuple> GroupbyOperator::next() { return dracula_imp_->next(); }

Operator::Counter GroupbyOperator::counters() const {
  return dracula_imp_->counters();
}
} // namespace vql::algebra
