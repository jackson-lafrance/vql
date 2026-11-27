#include <cstddef>
#include <iostream>
#include <set>
#include <string>
#include <tuple>
#include <utility>

template <typename... Fields> using Tuple = std::tuple<Fields...>;

template <typename... Fields> using Relation = std::set<Tuple<Fields...>>;

template <typename Predicate, typename... Fields>
Relation<Fields...> selection(Relation<Fields...>, Predicate);

template <typename... Fields, std::size_t... Indexes>
Relation<std::tuple_element_t<Indexes, Tuple<Fields...>>...>
    projection(Relation<Fields...>, std::index_sequence<Indexes...>);

template <typename... Fields>
Relation<Fields...> cross_product(Relation<Fields...>, Relation<Fields...>);

template <typename... Fields>
Relation<Fields...> rename(Relation<Fields...>, std::string, std::string);

template <typename... Fields>
Relation<Fields...> difference(Relation<Fields...>, Relation<Fields...>);

template <typename... Fields>
Relation<Fields...> unioner(Relation<Fields...>, Relation<Fields...>);

template <typename... Fields>
Relation<Fields...> intersect(Relation<Fields...>, Relation<Fields...>);

template <typename... Fields> void printRelation(Relation<Fields...>);

template <typename Tuple, std::size_t... Indexes>
void printTuple(const Tuple &, std::index_sequence<Indexes...>);

int main() {
  Relation<int, std::string, int> r1{std::make_tuple(1, "Bobby", 99),
                                     std::make_tuple(2, "Selsabeel", 88),
                                     std::make_tuple(3, "Moses", 77)};

  Relation<int, std::string, int> r2{std::make_tuple(2, "Selsabeel", 88),
                                     std::make_tuple(3, "Moses", 77)};

  printRelation(r1);

  auto condition1 = [](const auto &t) { return std::get<2>(t) > 80; };
  auto sel1 = selection(r1, condition1);
  printRelation(sel1);

  auto condition2 = [](const auto &t) { return std::get<1>(t) == "Moses"; };
  auto sel2 = selection(r1, condition2);
  printRelation(sel2);

  auto prj1 = projection(r1, std::index_sequence<0>{});
  printRelation(prj1);

  auto prj2 = projection(r1, std::index_sequence<0, 2>{});
  printRelation(prj2);
}

template <typename... Fields> void printRelation(Relation<Fields...> r) {
  constexpr std::size_t size = sizeof...(Fields);

  for (const auto &x : r) {
    printTuple(x, std::index_sequence_for<Fields...>{});
    std::cout << std::endl;
  }

  std::cout << std::endl;
}

template <typename Tuple, std::size_t... Indexes>
void printTuple(const Tuple &x, std::index_sequence<Indexes...>) {
  ((std::cout << std::get<Indexes>(x) << " | "), ...);
}

template <typename Predicate, typename... Fields>
Relation<Fields...> selection(Relation<Fields...> r, Predicate c) {
  for (auto it = r.begin(); it != r.end();) {
    if (c(*it)) {
      ++it;
    } else {
      it = r.erase(it);
    }
  }

  return r;
}

template <typename... Fields, std::size_t... Indexes>
Relation<std::tuple_element_t<Indexes, Tuple<Fields...>>...>
projection(Relation<Fields...> r, std::index_sequence<Indexes...>) {
  Relation<std::tuple_element_t<Indexes, Tuple<Fields...>>...> res;

  for (auto it = r.begin(); it != r.end(); ++it) {
    res.insert(std::make_tuple(std::get<Indexes>(*it)...));
  }

  return res;
};
