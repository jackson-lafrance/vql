#ifndef SORT_OPERATOR_H
#define SORT_OPERATOR_H

#include "buffer_pool.h"
#include "operator.h"
#include "temp_file.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace vql::algebra {
class SortOperator : public Operator {
public:
  SortOperator(std::unique_ptr<Operator> input,
               storage::BufferPool &buffer_pool, std::vector<SortKey> keys,
               std::size_t workspace_frames);

  const Schema &schema() const override;
  void open() override;
  void close() override;
  std::optional<Tuple> next() override;
  Counter counters() const override;

private:
  friend class ExceptOperator;
  friend class IntersectOperator;

  void release_current();
  std::optional<Tuple> restore_current();

  struct RecordPosition {
    std::size_t page_id;
    std::size_t offset;
    std::size_t length;
  };

  bool try_add_record(Page &page, std::size_t page_id,
                      std::span<RecordPosition> positions,
                      std::size_t &record_count, const Tuple &tuple,
                      std::size_t record_size);
  struct MergeInput {
    std::unique_ptr<storage::TempFileReader> reader;
    std::optional<std::span<const char>> record;
  };

  void generate_runs();
  void merge_runs();
  std::size_t write_merged_run(std::span<const std::size_t> files);
  void prepare_merge(std::span<const std::size_t> files);
  std::optional<std::size_t> smallest_input();
  void close_merge_inputs();
  void write_run(std::size_t workspace_file, std::size_t index_file,
                 std::size_t record_count);
  int compare_positions(std::size_t workspace_file, const RecordPosition &left,
                        const RecordPosition &right);
  RecordPosition read_position(const Page &index, std::size_t position) const;
  RecordPosition read_position(std::size_t index_file, std::size_t position);
  void write_position(Page &index, std::size_t position,
                      const RecordPosition &record) const;
  void write_position(std::size_t index_file, std::size_t position,
                      const RecordPosition &record);
  std::span<const char> record_bytes(const Page &page,
                                     const RecordPosition &record) const;
  std::size_t create_temp_file();
  void remove_temp_file(std::size_t file_id);
  void account_io(storage::BufferPool::Stats before);

  std::unique_ptr<Operator> input_;
  storage::BufferPool &buffer_pool_;
  std::vector<SortKey> keys_;
  std::size_t workspace_frames_;
  bool final_merge_prepared_{false};
  std::vector<std::size_t> run_files_;
  std::vector<std::size_t> owned_files_;
  std::vector<MergeInput> merge_inputs_;
  std::optional<std::size_t> pending_advance_;
  bool input_needs_close_{false};
};
} // namespace vql::algebra

#endif // SORT_OPERATOR_H
