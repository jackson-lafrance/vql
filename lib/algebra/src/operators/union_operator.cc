#include "operators/union_operator.h"

#include "operators/unified_operator.h"

#include <utility>

namespace vql::algebra {
UnionOperator::UnionOperator(std::unique_ptr<Operator> left_input,
                             std::unique_ptr<Operator> right_input,
                             storage::BufferPool &buffer_pool, Mode mode,
                             Options options)
    : distinct_(std::make_unique<UnifiedOperator>(std::move(left_input),
                                                 std::move(right_input)),
                buffer_pool, mode, options) {}

const Schema &UnionOperator::schema() const { return distinct_.schema(); }

void UnionOperator::open() { distinct_.open(); }

void UnionOperator::close() { distinct_.close(); }

std::optional<Tuple> UnionOperator::next() { return distinct_.next(); }

Operator::Counter UnionOperator::counters() const { return distinct_.counters(); }
} // namespace vql::algebra
