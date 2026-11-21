// RFC 0046 Stage-1: NSeatCardKeyerSeam branch-equivalence regression.
//
// The measurement seam must be behavior-invisible on the production path:
// with a null seam the trainer calls card_bucket(kNSeatCardKind, ...); with a
// seam that delegates to the SAME production function, two otherwise
// identical training runs must produce bitwise-identical raw tables. This
// pins the "nullptr path bit-for-bit unchanged" joint-review requirement in
// one binary (no cross-binary golden needed). It also checks the seam cap
// refusal when a keyer returns an out-of-cap bucket.
#include <algorithm>
#include <array>
#include <bit>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/nseat_trainer.hpp>
#include <cstdint>
#include <cstdio>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

namespace {

using namespace bs;
using namespace bs::poker;
using namespace bs::solver;

int card(const char* name) {
  return cardId(std::string(name));
}

GameDef tiny_river_def() {
  GameDef def{};
  def.player_count = 2;
  def.button = 1;
  def.big_blind = 1;
  def.stacks = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 2;
  def.board = {card("2c"), card("3d"), card("7h"), card("Ks"), card("9c")};
  def.board_size = 5;
  return def;
}

// Delegates, verbatim, to the production postflop key.
class ProductionDelegatingKeyer final : public NSeatCardKeyerSeam {
 public:
  std::uint32_t bucket(const std::array<int, 2>& hole, std::span<const int> board) const override {
    return static_cast<std::uint32_t>(abstraction::card_bucket(kNSeatCardKind, hole, board));
  }
  std::uint32_t bucket_cap() const override { return 1u << 20; }
};

// Always returns the cap: must be rejected.
class OutOfCapKeyer final : public NSeatCardKeyerSeam {
 public:
  std::uint32_t bucket(const std::array<int, 2>&, std::span<const int>) const override {
    return bucket_cap();
  }
  std::uint32_t bucket_cap() const override { return 16; }
};

std::vector<std::vector<WeightedHand>> uniform_ranges(const GameDef& def) {
  std::array<bool, 52> blocked{};
  for (int i = 0; i < def.board_size; ++i)
    blocked[def.board[i]] = true;
  std::vector<WeightedHand> hands;
  for (int a = 0; a < 52; ++a)
    for (int b = a + 1; b < 52; ++b)
      if (!blocked[a] && !blocked[b])
        hands.push_back(WeightedHand{{a, b}, 1.0});
  return {hands, hands};
}

using NSeatRawTable = std::map<NSeatInformationKey, NSeatRawRow>;

bool rows_bitwise_equal(const NSeatRawTable& a, const NSeatRawTable& b) {
  if (a.size() != b.size())
    return false;
  auto ib = b.begin();
  for (auto ia = a.begin(); ia != a.end(); ++ia, ++ib) {
    if (ia->first != ib->first)
      return false;
    const NSeatRawRow& ra = ia->second;
    const NSeatRawRow& rb = ib->second;
    if (ra.actions != rb.actions || ra.visits != rb.visits ||
        ra.regrets.size() != rb.regrets.size() || ra.sums.size() != rb.sums.size())
      return false;
    for (std::size_t i = 0; i < ra.regrets.size(); ++i)
      if (std::bit_cast<std::uint64_t>(ra.regrets[i]) !=
          std::bit_cast<std::uint64_t>(rb.regrets[i]))
        return false;
    for (std::size_t i = 0; i < ra.sums.size(); ++i)
      if (std::bit_cast<std::uint64_t>(ra.sums[i]) != std::bit_cast<std::uint64_t>(rb.sums[i]))
        return false;
  }
  return true;
}

bool test_null_and_delegating_seam_identical() {
  const GameDef def = tiny_river_def();
  const tree::AbstractTree tree(def, abstraction::ActionAbstraction::identity());
  const auto ranges = uniform_ranges(def);

  NSeatTrainerLimits limits;
  constexpr std::uint64_t kIters = 3000;
  constexpr std::uint64_t kSeed = 4320;

  const NSeatTrainingResult null_run =
      train_nseat(tree, ranges, kIters, kSeed, limits, nullptr, nullptr, nullptr, nullptr);
  const ProductionDelegatingKeyer keyer;
  const NSeatTrainingResult seam_run =
      train_nseat(tree, ranges, kIters, kSeed, limits, nullptr, nullptr, nullptr, &keyer);

  CHECK(null_run.completed_iterations == kIters);
  CHECK(seam_run.completed_iterations == kIters);
  CHECK(rows_bitwise_equal(null_run.raw_rows, seam_run.raw_rows));
  return true;
}

bool test_out_of_cap_rejected() {
  const GameDef def = tiny_river_def();
  const tree::AbstractTree tree(def, abstraction::ActionAbstraction::identity());
  const auto ranges = uniform_ranges(def);
  NSeatTrainerLimits limits;
  const OutOfCapKeyer keyer;
  bool threw = false;
  try {
    (void)train_nseat(tree, ranges, 100, 4320, limits, nullptr, nullptr, nullptr, &keyer);
  } catch (const std::runtime_error& e) {
    threw = std::string(e.what()).find("out-of-cap") != std::string::npos;
  }
  CHECK(threw);
  return true;
}

}  // namespace

int main() {
  if (!test_null_and_delegating_seam_identical()) {
    std::printf("rfc0046_keyer_seam: FAIL null/delegating equivalence\n");
    return 1;
  }
  std::printf("rfc0046_keyer_seam: pass null/delegating equivalence\n");
  if (!test_out_of_cap_rejected()) {
    std::printf("rfc0046_keyer_seam: FAIL cap refusal\n");
    return 1;
  }
  std::printf("rfc0046_keyer_seam: pass cap refusal\n");
  std::printf("rfc0046_keyer_seam: all tests passed\n");
  return 0;
}
