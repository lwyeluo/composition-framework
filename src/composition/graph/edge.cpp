#include <utility>

#include <composition/graph/edge.hpp>
#include <sstream>

namespace composition::graph {

edge_idx_t &operator++(edge_idx_t &i) {
  auto val = static_cast<typename std::underlying_type<edge_idx_t>::type>(i);
  i = edge_idx_t(++val);
  return i;
}

const edge_idx_t operator++(edge_idx_t &i, int) {
  edge_idx_t res(i);
  ++i;
  return res;
}

std::ostream &operator<<(std::ostream &out, const edge_idx_t &i) {
  out << static_cast<typename std::underlying_type<edge_idx_t>::type>(i);
  return out;
}

bool operator<(edge_idx_t lhs, edge_idx_t rhs) {
  using T = typename std::underlying_type<edge_idx_t>::type;
  return static_cast<T>(lhs) < static_cast<T>(rhs);
}

std::ostream &edge_t::operator<<(std::ostream &os) noexcept {
  os << this->index;
  return os;
}

bool edge_t::operator==(const edge_t &rhs) noexcept { return this->index == rhs.index; }

bool edge_t::operator!=(const edge_t &rhs) noexcept { return !(*this == rhs); }

edge_t::edge_t(
    edge_idx_t index,
    std::unordered_map<constraint::constraint_idx_t, std::shared_ptr<constraint::Constraint>> constraints) noexcept
    : index(index), constraints(std::move(constraints)) {}
} // namespace composition::graph