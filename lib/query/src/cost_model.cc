#include "cost_model.h"

#include <limits>
#include <stdexcept>

namespace vql::query {

std::size_t add_cost(std::size_t left, std::size_t right) {
  if (right > std::numeric_limits<std::size_t>::max() - left)
    throw std::overflow_error("Page cost addition overflow!");
  return left + right;
}

std::size_t multiply_cost(std::size_t left, std::size_t right) {
  if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left)
    throw std::overflow_error("Page cost multiplication overflow!");
  return left * right;
}

std::size_t cost_blocks(std::size_t pages, std::size_t block_pages) {
  if (block_pages == 0)
    throw std::invalid_argument("Cost block size cannot be zero!");
  return pages / block_pages + (pages % block_pages != 0);
}

CostModel::CostModel(std::size_t workspace_frames)
    : workspace_frames_(workspace_frames) {
  if (workspace_frames_ == 0)
    throw std::invalid_argument("Cost model needs at least one frame!");
}

std::size_t CostModel::frames() const { return workspace_frames_; }

void CostModel::require_workspace() const {
  if (workspace_frames_ < 3)
    throw std::invalid_argument(
        "Sort and join costs require at least three frames!");
}

std::size_t CostModel::seqscan(std::size_t pages) const { return pages; }

std::size_t CostModel::sort_passes(std::size_t pages) const {
  require_workspace();
  if (pages == 0)
    return 0;

  std::size_t runs = cost_blocks(pages, workspace_frames_);
  std::size_t passes = 1;
  while (runs > 1) {
    runs = cost_blocks(runs, workspace_frames_ - 1);
    ++passes;
  }
  return passes;
}

std::size_t CostModel::sort(std::size_t pages) const {
  std::size_t passes = sort_passes(pages);
  if (passes == 0)
    return 0;
  std::size_t repeated_pages = multiply_cost(pages, passes - 1);
  return add_cost(multiply_cost(2, repeated_pages), pages);
}

std::size_t CostModel::nested_loop_join(std::size_t outer_pages,
                                        std::size_t inner_pages) const {
  require_workspace();
  std::size_t blocks = cost_blocks(outer_pages, workspace_frames_ - 2);
  return add_cost(outer_pages, multiply_cost(blocks, inner_pages));
}

std::size_t CostModel::sort_merge_join(std::size_t left_pages,
                                       std::size_t right_pages) const {
  std::size_t sorting = add_cost(sort(left_pages), sort(right_pages));
  std::size_t merging = add_cost(left_pages, right_pages);
  return add_cost(sorting, merging);
}

std::size_t CostModel::hash_join(std::size_t left_pages,
                                 std::size_t right_pages) const {
  require_workspace();
  return multiply_cost(3, add_cost(left_pages, right_pages));
}

} // namespace vql::query
