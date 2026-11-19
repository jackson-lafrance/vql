#ifndef SORT_GROUPBY_OPERATOR
#define SORT_GROUPBY_OPERATOR

#include "buffer_pool.h"
#include "groupby_operator.h"
#include "operator.h"

namespace vql::algebra {
class SortGroupbyOperator : public Operator {
public:
  SortGroupbyOperator(std::unique_ptr<Operator> input,
                      storage::BufferPool &buffer_pool,
                      std::size_t workspace_frames);
};
} // namespace vql::algebra

#endif // SORT_GROUPBY_OPERATOR
