#ifndef SORT_GROUPBY_OPERATOR
#define SORT_GROUPBY_OPERATOR

#include "buffer_pool.h"
#include "groupby_operator.h"
#include "operator.h"
#include "operators/sort_operator.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace vql::algebra {
class SortGroupbyOperator : public Operator {
public:
  using Aggregate = GroupbyOperator::Aggregate;

  SortGroupbyOperator(std::unique_ptr<Operator> input,
                      storage::BufferPool &buffer_pool,
                      std::size_t workspace_frames,
                      std::vector<std::string> key_names,
                      std::vector<Aggregate> aggregates);

  const Schema &schema() const override;
  void open() override;
  void close() override;
  std::optional<Tuple> next() override;
  Counter counters() const override;

private:
  using OwnedValue = std::variant<std::int32_t, std::string>;
  struct AggregateState {
    std::int64_t sum{0};
    std::uint64_t count{0};
    bool initialized{false};
    OwnedValue value{std::int32_t{0}};
  };

  std::optional<Tuple> next_input();
  std::vector<OwnedValue> key_values(const Tuple &tuple) const;
  bool same_key(const Tuple &tuple, const std::vector<OwnedValue> &key);
  void update_aggregates(const Tuple &tuple,
                         std::vector<AggregateState> &states);
  std::optional<Tuple> emit_empty_global_group();
  Tuple finish_group(const std::vector<OwnedValue> &keys,
                    const std::vector<AggregateState> &states);
  storage::BufferPool &buffer_pool_;
  std::size_t workspace_frames_;
  std::vector<std::string> key_names_;
  std::vector<std::size_t> key_positions_;
  std::vector<Aggregate> aggregates_;
  std::vector<std::size_t> aggregate_positions_;
  std::unique_ptr<SortOperator> sorter_;
  std::optional<std::vector<char>> pending_record_;
  std::vector<char> output_record_;
  bool sorter_needs_close_{false};
  bool empty_global_emitted_{false};
  bool sleepy_{false};
};
} // namespace vql::algebra

#endif // SORT_GROUPBY_OPERATOR
