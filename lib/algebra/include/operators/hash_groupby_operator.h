#ifndef HASH_GROUPBY_OPERATOR
#define HASH_GROUPBY_OPERATOR

#include "buffer_pool.h"
#include "groupby_operator.h"
#include "operator.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace vql::algebra {
class HashGroupbyOperator : public Operator {
public:
  using Aggregate = GroupbyOperator::Aggregate;

  HashGroupbyOperator(std::unique_ptr<Operator> input,
                      storage::BufferPool &buffer_pool,
                      std::size_t partition_count,
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
  struct GroupState {
    std::vector<OwnedValue> keys;
    std::vector<AggregateState> aggregates;
    std::size_t memory_estimate{0};
  };

  void partition_input();
  bool prepare_next_batch();
  std::size_t create_temp_file();
  void remove_temp_file(std::size_t file_id);
  std::string make_group_key(const Tuple &tuple) const;
  std::vector<OwnedValue> key_values(const Tuple &tuple) const;
  std::size_t estimate_group_bytes(const Tuple &tuple,
                                   std::string_view encoded_key) const;
  void update_aggregates(const Tuple &tuple, GroupState &group);
  Tuple finish_group(const GroupState &group);
  std::optional<Tuple> emit_empty_global_group();
  void account_io(storage::BufferPool::Stats before);

  std::unique_ptr<Operator> input_;
  storage::BufferPool &buffer_pool_;
  std::size_t partition_count_;
  std::vector<std::string> key_names_;
  std::vector<std::size_t> key_positions_;
  std::vector<Aggregate> aggregates_;
  std::vector<std::size_t> aggregate_positions_;
  std::vector<std::size_t> pending_files_;
  std::vector<std::size_t> owned_files_;
  std::optional<std::size_t> staging_file_id_;
  std::vector<GroupState> groups_;
  std::unordered_map<std::string, std::size_t> group_index_;
  std::size_t group_memory_used_{0};
  std::size_t output_position_{0};
  std::vector<char> output_record_;
  bool input_needs_close_{false};
  bool empty_global_emitted_{false};
  bool sleepy_{false};
};
} // namespace vql::algebra

#endif // HASH_GROUPBY_OPERATOR
