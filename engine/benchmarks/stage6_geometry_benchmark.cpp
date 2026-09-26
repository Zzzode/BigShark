// RFC 0008 stage 6 R3b/R12-step1: flop-geometry matrix and coarse-tree sizing
// measurement. Manual benchmark (NOT a timing-gated CTest). Enumerates the
// reachable flop geometries at each required seat count and, when --trees is
// passed, builds each signature's coarse rooted-flop AbstractTree to record
// node/infoset/byte counts against the TreeLimits caps. Prints a deterministic
// report that is captured into the frozen config and the stage-6 evidence.
#include <algorithm>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/stage6/geometry.hpp>
#include <bs/stage6/geometry_enumerator.hpp>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

using namespace bs::poker;
using namespace bs::stage6;
using bs::abstraction::ActionAbstraction;
using bs::abstraction::SizeSchedule;
using bs::tree::AbstractTree;
using bs::tree::tree_resource_exhausted;

namespace {

// Builds the REDUCED rooted-flop GameDef for an actionable bucket: only the
// live_count seats are present (folded seats cannot be represented in a
// rooted GameDef), acting seats carry the min acting stack behind and every
// seat contributes its reconciled dead share.
GameDef rooted_def_for(const GeometryBucket& bucket, const std::array<int, 3>& board) {
  const std::size_t live = bucket.key.live_count;
  GameDef def{};
  def.player_count = live;
  def.button = 0;
  def.big_blind = 2;
  def.preflop = false;
  Chips total = 0;
  for (std::size_t p = 0; p < live; ++p) {
    def.stacks[p] = bucket.representative_stacks[p];
    def.contributions[p] = bucket.representative_contrib[p];
    total += bucket.representative_contrib[p];
  }
  def.pot = bucket.representative_pot;
  if (total != def.pot)
    throw std::runtime_error("representative dead contributions do not reconcile to pot");
  def.board = {board[0], board[1], board[2], 0, 0};
  def.board_size = 3;
  return def;
}

int measure_seats(std::size_t n, bool build_trees, bool deviation, bool dump, std::size_t only_n) {
  if (only_n != 0 && n != only_n)
    return 0;
  std::vector<std::string> lines;
  const std::vector<GeometrySignature> sigs = deviation
                                                  ? enumerate_flop_geometries(n, 2, &lines)
                                                  : enumerate_chart_flop_geometries(n, 2, &lines);
  std::printf("seats=%zu %s exact-geometries=%zu\n", n, deviation ? "deviation" : "chart-only",
              sigs.size());
  if (dump)
    for (const GeometrySignature& s : sigs)
      std::printf("  %s\n", s.to_string().c_str());

  const std::vector<GeometryBucket> buckets = bucket_geometries(n, 2, sigs);
  std::size_t actionable = 0;
  std::size_t runout = 0;
  for (const GeometryBucket& b : buckets)
    (b.actionable ? actionable : runout)++;
  std::printf("  buckets=%zu actionable=%zu all-in-runout=%zu\n", buckets.size(), actionable,
              runout);

  if (dump)
    for (const GeometryBucket& bk : buckets) {
      std::printf("  BUCKET %s actionable=%d minstack=%llu pot=%llu", bk.key.to_string().c_str(),
                  static_cast<int>(bk.actionable),
                  static_cast<unsigned long long>(bk.min_acting_stack),
                  static_cast<unsigned long long>(bk.representative_pot));
      if (!bk.actionable) {
        std::printf(" runout\n");
        continue;
      }
      std::printf(" st[");
      for (std::size_t i = 0; i < bk.key.live_count; ++i)
        std::printf("%s%llu", i ? "," : "",
                    static_cast<unsigned long long>(bk.representative_stacks[i]));
      std::printf("] in[");
      for (std::size_t i = 0; i < bk.key.live_count; ++i)
        std::printf("%s%llu", i ? "," : "",
                    static_cast<unsigned long long>(bk.representative_contrib[i]));
      std::printf("]\n");
    }

  if (!build_trees)
    return 0;
  // Coarse postflop menu: one bet + one raise per street, declared targets
  // only. The independent wall review (2026-09-24) established that the
  // identity builder's forced minimum/cap seeds -- not card fan-out -- were
  // the deep-tree explosion, so the coarse menu MUST use DeclaredOnly.
  SizeSchedule coarse = bs::abstraction::default_size_schedule();
  for (auto& street : coarse) {
    street.bets = {{1, 2}};
    street.raises = {{1, 1}};
  }
  ActionAbstraction action =
      ActionAbstraction::declared(coarse, bs::abstraction::CoverSeeds::DeclaredOnly);
  std::printf("coarse-abstraction=%s\n", action.id().to_string().c_str());
  // Representative board 2c 3d 7h (ids 0,6,21), as in the pinned 3p fixture.
  const std::array<int, 3> board = {{0, 6, 21}};
  std::size_t total_nodes = 0;
  std::size_t max_nodes = 0;
  std::size_t capped = 0;
  std::size_t built = 0;
  for (const GeometryBucket& bucket : buckets) {
    if (!bucket.actionable)
      continue;  // all-in-at-flop runout: no policy tree
    const GameDef def = rooted_def_for(bucket, board);
    try {
      AbstractTree tree(def, action);
      total_nodes += tree.size();
      max_nodes = std::max(max_nodes, tree.size());
      ++built;
    } catch (const tree_resource_exhausted&) {
      ++capped;
      std::printf("  CAPPED: %s\n", bucket.key.to_string().c_str());
    }
  }
  std::printf("  trees built=%zu capped=%zu total_nodes=%zu max_nodes=%zu\n", built, capped,
              total_nodes, max_nodes);
  return capped == 0 ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
  bool build_trees = false;
  bool deviation = false;
  bool dump = false;
  std::size_t only_n = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--trees")
      build_trees = true;
    else if (arg == "--deviation")
      deviation = true;
    else if (arg == "--dump")
      dump = true;
    else if (arg == "--n" && i + 1 < argc)
      only_n = std::stoul(argv[++i]);
  }
  int rc = 0;
  for (std::size_t n : {std::size_t{2}, std::size_t{3}, std::size_t{6}, std::size_t{7},
                        std::size_t{9}, std::size_t{10}})
    rc = measure_seats(n, build_trees, deviation, dump, only_n) || rc;
  return rc;
}
