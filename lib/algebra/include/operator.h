#ifndef OPERATOR_H
#define OPERATOR_H

#include "schema.h"
#include "tuple.h"
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vql::algebra {
class Operator {
protected:
  struct Counter {
    std::size_t tuples_out;
    std::size_t comparisons;
    std::size_t page_reads;
    std::size_t page_writes;
  };

public:
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
  Counter counter_{0, 0, 0, 0};
  Schema schema_{std::vector<Column>{}};
};

class ProjectionOperator : public Operator {
public:
  ProjectionOperator(std::unique_ptr<Operator> input,
                     std::vector<std::string> column_names);

  const Schema &schema() const override;
  void open() override;
  void close() override;
  std::optional<Tuple> next() override;
  Counter counters() const override;

private:
  std::unique_ptr<Operator> input_;
  std::vector<std::string> column_names_;
  std::vector<std::size_t> positions_;
};

class FilterOperator : public Operator {
public:
  FilterOperator(std::unique_ptr<Operator> input,
                 std::function<bool(const Tuple &)> condition);
  const Schema &schema() const override;
  void open() override;
  void close() override;
  std::optional<Tuple> next() override;
  Counter counters() const override;

private:
  std::unique_ptr<Operator> input_;
  std::function<bool(const Tuple &)> condition_;
};

} // namespace vql::algebra

#endif // OPERATOR_H
