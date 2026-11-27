#ifndef VQL_SHELL_H
#define VQL_SHELL_H

#include "buffer_pool.h"
#include "catalog.h"

#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>

namespace vql {

class Shell {
public:
  Shell(std::filesystem::path directory, std::size_t frames);
  int run(std::istream &input, std::ostream &output, std::ostream &errors,
          bool prompt);

private:
  void execute(const std::string &command, std::ostream &output);
  void resize(std::size_t frames);

  std::filesystem::path directory_;
  std::unique_ptr<storage::BufferPool> pool_;
  std::unique_ptr<storage::Catalog> catalog_;
};

} // namespace vql
#endif
