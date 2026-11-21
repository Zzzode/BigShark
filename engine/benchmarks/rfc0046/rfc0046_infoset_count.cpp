// RFC 0046 Stage-1 measurement-only concrete-infoset counter (manual
// benchmark, NOT a CTest, not a gate row). Answers the V3 envelope question
// WITHOUT solving: an exact-key turn-rooted cap-1 solver must allocate one
// infoset per (public action node, reachable own combo). For a full uniform
// range with the four board cards removed there are C(48,2)=1128 live turn
// combos; a river action node sits under one dealt river card r, which blocks
// the 47 pairs containing r, leaving C(47,2)=1081 combos. Betting actions are
// public and hole-card-independent, so every live combo reaches every action
// node of its street under any fully-mixed reach - this count is the exact
// fully-mixed concrete-infoset footprint, not a sampled estimate.
//
//   bigshark-rfc0046-infoset-count [stratum ...]   (default: all six)
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <cstdio>
#include <string_view>

#include "strata.hpp"

using namespace bs;
using namespace bs::rfc0046;

namespace {

constexpr long long kTurnCombos = 48LL * 47 / 2;   // 1128
constexpr long long kRiverCombos = 47LL * 46 / 2;  // 1081

void count_stratum(const StratumSpec& spec, std::uint8_t cap) {
  const poker::GameDef def = make_stratum_def(spec, cap);
  const tree::AbstractTree tree(def, abstraction::ActionAbstraction::identity(cap));

  std::size_t turn_action = 0;
  std::size_t river_action = 0;
  std::size_t chance = 0;
  std::size_t terminal = 0;
  for (const tree::TreeNode& node : tree.nodes()) {
    if (node.is_action()) {
      if (node.street == poker::Street::Turn)
        ++turn_action;
      else if (node.street == poker::Street::River)
        ++river_action;
    } else if (node.is_chance()) {
      ++chance;
    } else if (node.is_terminal()) {
      ++terminal;
    }
  }

  const std::size_t range_size = stratum_range(spec).size();
  const long long full_infosets = static_cast<long long>(turn_action) * kTurnCombos +
                                  static_cast<long long>(river_action) * kRiverCombos;
  const long long envelope = 1'000'000;
  // Strict LOWER bound on retained footprint: excludes the ~1,128 responder
  // -x gadget root rows, the per-row action/regret/sum arrays, and map/node
  // overhead. The OVER verdict is therefore conservative.
  std::printf(
      "%-24s cap=%u nodes=%zu turn_act=%zu river_act=%zu chance=%zu term=%zu | "
      "full_infosets=%lld (%s 1M) range_size=%zu\n",
      spec.name, static_cast<unsigned>(cap), tree.size(), turn_action, river_action, chance,
      terminal, full_infosets, full_infosets > envelope ? "OVER" : "under", range_size);
}

}  // namespace

int main(int argc, char** argv) {
  std::printf("# full-range exact-key concrete infosets = turn_act*1128 + river_act*1081\n");
  if (argc > 1) {
    for (int i = 1; i < argc; ++i) {
      const StratumSpec* spec = find_stratum(argv[i]);
      if (!spec) {
        std::fprintf(stderr, "unknown stratum %s\n", argv[i]);
        return 2;
      }
      count_stratum(*spec, 1);
    }
    return 0;
  }
  for (const StratumSpec& spec : kStrata)
    count_stratum(spec, 1);
  return 0;
}
