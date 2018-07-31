#ifndef COMPOSITION_FRAMEWORK_GRAPH_FILTER_REMOVED_HPP
#define COMPOSITION_FRAMEWORK_GRAPH_FILTER_REMOVED_HPP

#include <composition/graph/filter/filter.hpp>

namespace composition {
template<typename T>
struct RemovedPredicate { // both edge and vertex
  bool operator()(typename T::edge_descriptor ed) const {
    assert(G != nullptr);
    return !(*G)[ed].removed;
  }

  bool operator()(typename T::vertex_descriptor vd) const {
    assert(G != nullptr);
    return !(*G)[vd].removed;
  }

  T *G;

  RemovedPredicate() = default;

  explicit RemovedPredicate(T &G) : G(&G) {}
};

template<typename graph_t>
auto filter_removed_graph(graph_t &g) -> decltype(filter_graph<RemovedPredicate>(g)) {
  return filter_graph<RemovedPredicate>(g);
}
}
#endif //COMPOSITION_FRAMEWORK_GRAPH_FILTER_REMOVED_HPP
