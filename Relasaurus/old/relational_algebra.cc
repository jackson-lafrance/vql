#include <iostream>
#include <set>
#include <string>
#include <tuple>

using namespace std;

template <typename... Fields> using Tuple = tuple<Fields...>;

template <typename... Fields> using Relation = set<Tuple<Fields...>>;

template <typename Predicate, typename... Fields>
Relation<Fields...> selection(Relation<Fields...>, Predicate);
template <typename... Fields, size_t... Indexes>
Relation<tuple_element_t<Indexes, Tuple<Fields...>>...>
    projection(Relation<Fields...>, index_sequence<Indexes...>);
template <typename... Fields>
Relation<Fields...> cross_product(Relation<Fields...>, Relation<Fields...>);
template <typename... Fields>
Relation<Fields...> rename(Relation<Fields...>, string, string);
template <typename... Fields>
Relation<Fields...> difference(Relation<Fields...>, Relation<Fields...>);
template <typename... Fields>
Relation<Fields...> unioner(Relation<Fields...>, Relation<Fields...>);
template <typename... Fields>
Relation<Fields...> intersect(Relation<Fields...>, Relation<Fields...>);

template <typename... Fields> void printRelation(Relation<Fields...>);
template <typename Tuple, size_t... Indexes>
void printTuple(const Tuple &, index_sequence<Indexes...>);

int main() {
  Relation<int, string, int> r1{make_tuple(1, "Bobby", 99),
                                make_tuple(2, "Selsabeel", 88),
                                make_tuple(3, "Moses", 77)};

  Relation<int, string, int> r2{make_tuple(2, "Selsabeel", 88),
                                make_tuple(3, "Moses", 77)};

  printRelation(r1);

  auto condition1 = [](const auto &t) { return get<2>(t) > 80; };
  auto sel1 = selection(r1, condition1);
  printRelation(sel1);

  auto condition2 = [](const auto &t) { return get<1>(t) == "Moses"; };
  auto sel2 = selection(r1, condition2);
  printRelation(sel2);

  auto prj1 = projection(r1, index_sequence<0>{});
  printRelation(prj1);

  auto prj2 = projection(r1, index_sequence<0, 2>{});
  printRelation(prj2);
}

template <typename... Fields> void printRelation(Relation<Fields...> r) {
  constexpr size_t size = sizeof...(Fields);
  for (const auto &x : r) {
    printTuple(x, index_sequence_for<Fields...>{});
    cout << endl;
  }

  cout << endl;
}

template <typename Tuple, size_t... Indexes>
void printTuple(const Tuple &x, index_sequence<Indexes...>) {
  ((cout << get<Indexes>(x) << " | "), ...);
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

template <typename... Fields, size_t... Indexes>
Relation<tuple_element_t<Indexes, Tuple<Fields...>>...>
projection(Relation<Fields...> r, index_sequence<Indexes...>) {

  Relation<tuple_element_t<Indexes, Tuple<Fields...>>...> res;

  for (auto it = r.begin(); it != r.end(); ++it) {
    res.insert(make_tuple(get<Indexes>(*it)...));
  }

  return res;
};
