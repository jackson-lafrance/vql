#ifndef VQL_QUERY_COST_MODEL_H
#define VQL_QUERY_COST_MODEL_H

#include <cstddef>

namespace vql::query {

class CostModel {
public:
  explicit CostModel(std::size_t workspace_frames);

  std::size_t frames() const;
  std::size_t seqscan(std::size_t pages) const;
  std::size_t sort_passes(std::size_t pages) const;
  std::size_t sort(std::size_t pages) const;
  std::size_t nested_loop_join(std::size_t outer_pages,
                               std::size_t inner_pages) const;
  std::size_t sort_merge_join(std::size_t left_pages,
                              std::size_t right_pages) const;
  std::size_t hash_join(std::size_t left_pages, std::size_t right_pages) const;

private:
  void require_workspace() const;

  std::size_t workspace_frames_;
};

} // namespace vql::query

#endif // VQL_QUERY_COST_MODEL_H
