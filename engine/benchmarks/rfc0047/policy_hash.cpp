#include "policy_hash.hpp"

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace bs::rfc0047 {

namespace {
constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

void hash_bytes(std::uint64_t& state, const void* data, std::size_t bytes) {
  const auto* p = static_cast<const unsigned char*>(data);
  for (std::size_t k = 0; k < bytes; ++k) {
    state ^= static_cast<std::uint64_t>(p[k]);
    state *= kFnvPrime;
  }
}
}  // namespace

std::uint64_t hash_average_snapshot(const std::vector<std::vector<double>>& avg_sigma) {
  std::uint64_t h = kFnvOffsetBasis;
  // DFS node order is the outer vector's index order by construction.
  for (std::size_t n = 0; n < avg_sigma.size(); ++n) {
    const std::vector<double>& row = avg_sigma[n];
    if (row.empty())
      continue;
    hash_bytes(h, &n, sizeof(n));  // bind each row to its node position
    hash_bytes(h, row.data(), row.size() * sizeof(double));
  }
  return h;
}

double row_l1_distance(const std::vector<double>& p, const std::vector<double>& q) {
  if (p.size() != q.size())
    throw std::invalid_argument("policy_hash: row widths differ");
  double d = 0.0;
  for (std::size_t k = 0; k < p.size(); ++k)
    d += std::fabs(p[k] - q[k]);
  return d;
}

double max_snapshot_l1(const std::vector<std::vector<double>>& linear,
                       const std::vector<std::vector<double>>& uniform) {
  if (linear.size() != uniform.size())
    throw std::invalid_argument("policy_hash: snapshot node counts differ");
  constexpr std::uint32_t kCombos = 1081;
  double best = 0.0;
  for (std::size_t n = 0; n < linear.size(); ++n) {
    if (linear[n].empty() != uniform[n].empty())
      throw std::invalid_argument("policy_hash: empty-node pattern differs");
    if (linear[n].empty())
      continue;
    if (linear[n].size() % kCombos != 0)
      throw std::invalid_argument("policy_hash: row is not combo-row-major");
    const std::size_t menu = linear[n].size() / kCombos;
    if (uniform[n].size() != menu * kCombos)
      throw std::invalid_argument("policy_hash: combo/menu shape differs");
    for (std::uint32_t i = 0; i < kCombos; ++i) {
      double d = 0.0;
      for (std::size_t a = 0; a < menu; ++a) {
        const std::size_t k = static_cast<std::size_t>(i) * menu + a;
        d += std::fabs(linear[n][k] - uniform[n][k]);
      }
      best = std::max(best, d);  // per-combo metric, bounded in [0,2]
    }
  }
  return best;
}

}  // namespace bs::rfc0047
