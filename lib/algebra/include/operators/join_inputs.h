#ifndef JOIN_INPUTS_H
#define JOIN_INPUTS_H

#include "buffer_pool.h"
#include "operator.h"
#include "temp_file.h"

#include <cstddef>
#include <memory>
#include <optional>

namespace vql::algebra::detail {
// Saves child output for a join. This helper is not itself an Operator.
class JoinInputs {
public:
  JoinInputs(std::unique_ptr<Operator> left, std::unique_ptr<Operator> right,
             storage::BufferPool &buffer_pool);
  ~JoinInputs();

  JoinInputs(const JoinInputs &) = delete;
  JoinInputs &operator=(const JoinInputs &) = delete;

  const Schema &schema() const;
  const Schema &left_schema() const;
  const Schema &right_schema() const;

  // Opens, drains, and closes the left child, then does the same for the right.
  void open();
  // Idempotent; also cleans up after an unsuccessful open. Readers must be closed.
  void close();

  std::size_t left_file_id() const;
  std::size_t right_file_id() const;
  // Only this helper's I/O, not child I/O. No join rows are emitted here.
  Operator::Counter counters() const;
  Operator::Counter left_counters() const;
  Operator::Counter right_counters() const;

private:
  enum class State { CREATED, OPEN, FAILED, CLOSED };

  void save_input(Operator &input, std::optional<std::size_t> &file_id,
                  bool &needs_close);
  void append_tuple(storage::TempFile &writer, const Tuple &tuple,
                    const Schema &schema);
  std::size_t create_temp_file();
  void remove_file(std::optional<std::size_t> &file_id);
  void account_io(storage::BufferPool::Stats before);

  std::unique_ptr<Operator> left_;
  std::unique_ptr<Operator> right_;
  storage::BufferPool &buffer_pool_;
  Schema schema_;
  std::optional<std::size_t> left_file_id_;
  std::optional<std::size_t> right_file_id_;
  // A buffer-pool page for encoding projected rows, never a growing row list.
  std::optional<std::size_t> encoding_file_id_;
  bool left_needs_close_{false};
  bool right_needs_close_{false};
  State state_{State::CREATED};
  Operator::Counter counter_{};
};
} // namespace vql::algebra::detail

#endif // JOIN_INPUTS_H
