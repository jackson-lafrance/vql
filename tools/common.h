#ifndef VQL_TOOLS_COMMON_H
#define VQL_TOOLS_COMMON_H

#include "buffer_pool.h"
#include "catalog.h"
#include "tuple.h"

#include <functional>
#include <json/json.h>
#include <map>
#include <string>

namespace vql::cli {

class Args {
public:
  Args(int argc, char **argv);

  std::string command;

  bool has(const std::string &key) const;
  std::string get(const std::string &key,
                  const std::string &fallback = "") const;
  std::string require(const std::string &key) const;
  std::size_t number(const std::string &key, std::size_t fallback) const;
  void allow(std::initializer_list<const char *> keys) const;

private:
  std::map<std::string, std::string> options_;
};

std::size_t unsigned_number(const std::string &text, const std::string &label);
std::string read_text(const std::string &path);
Json::Value read_json(const std::string &path);
std::string json(const Json::Value &value);

Json::Value schema_json(const algebra::Schema &schema);
Json::Value row_json(const algebra::Tuple &tuple);
Json::Value stats_json(const storage::BufferPool::Stats &stats);

struct Database {
  storage::BufferPool pool;
  storage::Catalog catalog;

  explicit Database(const Args &args);
};

int guarded(const std::function<void()> &run);

} // namespace vql::cli

#endif
