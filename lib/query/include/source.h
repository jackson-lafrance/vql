#ifndef VQL_QUERY_SOURCE_H
#define VQL_QUERY_SOURCE_H

#include <cstddef>

namespace vql::query {

struct Position {
  std::size_t offset{0}; 
  std::size_t row{1};
  std::size_t col{1};
};

struct Span {
  Position begin;
  Position end; 
};

} // namespace vql::query

#endif // VQL_QUERY_SOURCE_H
