#ifndef OPERATOR_H
#define OPERATOR_H

#include "schema.h"
#include "tuple.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace vql::algebra {
class Operator {
public:
  struct Counter {
    std::size_t tuples_out{0};
    std::size_t comparisons{0};
    std::size_t page_reads{0};
    std::size_t page_writes{0};
  };

  virtual ~Operator() = default;
  virtual const Schema &schema() const = 0;
  virtual void open() = 0;
  virtual void close() = 0;
  virtual std::optional<Tuple> next() = 0;
  virtual Counter counters() const = 0;

protected:
  enum class State { CREATED, OPEN, FAILED, CLOSED };

  Operator() = default;

  State state_{State::CREATED};
  Counter counter_{};
  Schema schema_{std::vector<Column>{}};
};
} // namespace vql::algebra

#endif // OPERATOR_H
