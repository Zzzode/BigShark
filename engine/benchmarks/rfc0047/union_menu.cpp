#include "union_menu.hpp"

#include <algorithm>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <stdexcept>

namespace bs::rfc0047 {

using bs::abstraction::ActionAbstraction;
using bs::abstraction::CoverSeeds;
using bs::abstraction::Fraction;
using bs::abstraction::SizeSchedule;
using bs::abstraction::StreetSizes;
using bs::poker::Action;
using bs::poker::ActionType;
using bs::poker::Street;
using bs::tree::AbstractTree;
using bs::tree::TreeNode;

namespace {

// Strict value order on reduced pot fractions. Equality/dedup uses the exact
// defaulted Fraction operator== (declared fractions are reduced at
// construction, so equal values share one reduced form); long double only
// orders DISTINCT fractions, where the engine's small validated pot fractions
// cannot approach its 64-bit mantissa.
bool fraction_less(const Fraction& a, const Fraction& b) {
  const long double left =
      static_cast<long double>(a.numerator) / static_cast<long double>(a.denominator);
  const long double right =
      static_cast<long double>(b.numerator) / static_cast<long double>(b.denominator);
  return left < right;
}

std::vector<Fraction> merge_fractions(const std::vector<Fraction>& a,
                                      const std::vector<Fraction>& b) {
  std::vector<Fraction> merged;
  merged.reserve(a.size() + b.size());
  merged.insert(merged.end(), a.begin(), a.end());
  merged.insert(merged.end(), b.begin(), b.end());
  std::sort(merged.begin(), merged.end(), fraction_less);
  merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
  return merged;
}

bool is_aggressive(ActionType type) {
  return type == ActionType::Bet || type == ActionType::Raise;
}

// Paired DFS for the generalized subset map. Reference node nr is paired with
// bounded node nb; reference-only subtrees stay at the kNoNode default.
void pair_subset_dfs(const AbstractTree& ref, const AbstractTree& bounded, std::size_t nr,
                     std::size_t nb, SubsetMap& out) {
  out.node_map[nr] = nb;
  const TreeNode& r = ref.node(nr);
  const TreeNode& b = bounded.node(nb);
  // Defensive depth guard (review advisory L1): same-GameDef plus exact-action
  // induction makes a mismatch impossible, but fail closed rather than pair
  // incompatible nodes.
  if (r.kind != b.kind)
    throw std::invalid_argument("union_menu: paired nodes diverge in kind");
  if (!r.is_action() || !b.is_action())
    return;  // terminals/chance pair only at the node itself
  if (b.actions.size() > r.actions.size())
    throw std::invalid_argument("union_menu: bounded menu cannot exceed reference menu");

  out.edge_map[nr].assign(r.actions.size(), -1);
  for (std::size_t ab = 0; ab < b.actions.size(); ++ab) {
    auto it = std::find(r.actions.begin(), r.actions.end(), b.actions[ab]);
    if (it == r.actions.end())
      throw std::invalid_argument("union_menu: bounded action is not in the reference menu");
    const std::size_t ar = static_cast<std::size_t>(it - r.actions.begin());
    if (out.edge_map[nr][ar] != -1)
      throw std::invalid_argument("union_menu: duplicate bounded action match");
    out.edge_map[nr][ar] = static_cast<int>(ab);
  }
  for (std::size_t ar = 0; ar < r.actions.size(); ++ar) {
    if (out.edge_map[nr][ar] != -1)
      continue;
    // A reference-only passive edge cannot exist at an equal state: passive
    // actions derive solely from legality and are identical in both menus.
    if (!is_aggressive(r.actions[ar].type))
      throw std::invalid_argument("union_menu: reference-only passive edge indicates state drift");
    // The whole extension subtree keeps the kNoNode default by construction.
  }
  for (std::size_t ar = 0; ar < r.actions.size(); ++ar) {
    const int ab = out.edge_map[nr][ar];
    if (ab != -1)
      pair_subset_dfs(ref, bounded, r.children[ar], b.children[static_cast<std::size_t>(ab)], out);
  }
}

}  // namespace

ActionAbstraction make_union_abstraction(const ActionAbstraction& first,
                                         const ActionAbstraction& second) {
  if (first.cover_seeds() != CoverSeeds::MinAndCap || second.cover_seeds() != CoverSeeds::MinAndCap)
    throw std::invalid_argument("union_menu: union requires MinAndCap cover seeds on both inputs");

  SizeSchedule schedule = first.schedule();
  const SizeSchedule& other = second.schedule();
  for (Street st : {Street::Flop, Street::Turn, Street::River, Street::Preflop}) {
    const auto idx = static_cast<std::size_t>(st);
    StreetSizes& dst = schedule[idx];
    dst.bets = merge_fractions(dst.bets, other[idx].bets);
    dst.raises = merge_fractions(dst.raises, other[idx].raises);
  }
  // Always uncapped and MinAndCap: the union reference contains the full
  // identity(0) menu and mints its own declared-family digest.
  return ActionAbstraction::declared(std::move(schedule), CoverSeeds::MinAndCap, 0);
}

SubsetMap map_bounded_subset(const AbstractTree& reference, const AbstractTree& bounded) {
  if (!bs::poker::same_game_def(reference.def(), bounded.def()))
    throw std::invalid_argument(
        "union_menu: reference and bounded trees need the identical GameDef");
  SubsetMap out;
  out.node_map.assign(reference.size(), SubsetMap::kNoNode);
  out.edge_map.resize(reference.size());
  pair_subset_dfs(reference, bounded, reference.root_index(), bounded.root_index(), out);
  return out;
}

}  // namespace bs::rfc0047
