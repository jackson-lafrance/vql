#include "tuple.h"

namespace vql::algebra {

bool operator==(const Value &left, const Value &right) { return false; }
bool operator!=(const Value &left, const Value &right) { return false; }
bool operator<(const Value &left, const Value &right) { return false; }

bool operator==(const Tuple &left, const Tuple &right) { return false; }
bool operator!=(const Tuple &left, const Tuple &right) { return false; }
bool operator<(const Tuple &left, const Tuple &right) { return false; }

} // namespace vql::algebra
