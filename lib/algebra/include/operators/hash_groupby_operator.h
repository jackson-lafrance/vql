#ifndef HASH_GROUPBY_OPERATOR
#define HASH_GROUPBY_OPERATOR

#include "buffer_pool.h"
#include "groupby_operator.h"
#include "operator.h"

namespace vql::algebra {
class HashGroupbyOperator : public Operator {
public:
  HashGroupbyOperator(std::unique_ptr<Operator> input,
                      storage::BufferPool &buffer_pool,
                      std::size_t partition_count);
};
} // namespace vql::algebra

#endif // HASH_GROUPBY_OPERATOR
