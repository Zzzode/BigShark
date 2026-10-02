// RFC 0009 W4c-iii: flop class library builder tests.
//
// Verifies the class enumeration, the declared range, the coverage counter,
// and that build_library produces schema-v3 artifacts whose declared card
// abstraction is the suit-canonicalization id, with honest manifest
// measurements. The resident v3 lookup path itself is tested in
// test_resident_policy (W4c-ii).
#include <array>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <bs/flop_library.hpp>
#include <bs/heads_up.hpp>
#include <bs/strategy_artifact.hpp>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

namespace {

namespace fs = std::filesystem;

bool test_enumerate_classes() {
  const auto classes = bs::flop_library::enumerate_classes();
  CHECK(classes.size() == 1755);
  // std::set yields sorted, unique representatives.
  for (std::size_t i = 1; i < classes.size(); ++i) {
    CHECK(classes[i - 1] < classes[i]);
  }
  // Every returned board is its own canonical representative.
  for (const auto& b : classes) {
    const bs::abstraction::CanonicalBoard canon = bs::abstraction::canonicalize(b);
    CHECK(canon.board == b);
  }
  return true;
}

bool test_declared_range() {
  const auto range = bs::flop_library::declared_library_range();
  CHECK(range.size() == 40);
  // Pin the exact content: 4 pocket pairs (6 combos each) + AKs (4) + AKo
  // (12) = 40. If every expected combo is present and the size is 40, the
  // content is exactly the declared premium set.
  auto has = [&](int c0, int c1) {
    for (const auto& h : range)
      if (h.cards[0] == c0 && h.cards[1] == c1)
        return true;
    return false;
  };
  // AA, KK, QQ, JJ (ranks 12, 11, 10, 9): all 6 combos each.
  for (int r : {12, 11, 10, 9})
    for (int s0 = 0; s0 < 4; ++s0)
      for (int s1 = s0 + 1; s1 < 4; ++s1)
        CHECK(has(r * 4 + s0, r * 4 + s1));
  // AKs: K (rank 11) is the lower card, A (rank 12) the higher, same suit.
  for (int s = 0; s < 4; ++s)
    CHECK(has(11 * 4 + s, 12 * 4 + s));
  // AKo: K lower, A higher, different suits.
  for (int s0 = 0; s0 < 4; ++s0)
    for (int s1 = 0; s1 < 4; ++s1)
      if (s1 != s0)
        CHECK(has(11 * 4 + s1, 12 * 4 + s0));
  for (const auto& h : range) {
    CHECK(h.cards[0] < h.cards[1]);
    CHECK(h.cards[0] >= 0 && h.cards[1] < 52);
    CHECK(h.weight == 1.0);
  }
  return true;
}

bool test_coverage() {
  const auto classes = bs::flop_library::enumerate_classes();
  CHECK(bs::flop_library::count_covered_flops({}) == 0);
  CHECK(bs::flop_library::count_covered_flops(classes) == 22100);
  const std::size_t first = bs::flop_library::count_covered_flops({classes[0]});
  CHECK(first > 0);
  CHECK(first <= 22100);
  // Coverage is monotone: adding a class never decreases the count.
  const std::size_t two = bs::flop_library::count_covered_flops({classes[0], classes[1]});
  CHECK(two >= first);
  return true;
}

bool test_build_library(const fs::path& dir) {
  const auto range = bs::flop_library::declared_library_range();
  // Shallow stacks (4 behind, 2 in per seat, pot 4) keep the tree small.
  const bs::flop_library::LibraryManifest manifest =
      bs::flop_library::build_library(dir, 2, 100, 20261002, 4, 2, range);

  CHECK(manifest.class_count == 2);
  CHECK(manifest.total_classes == 1755);
  CHECK(manifest.total_flops == 22100);
  CHECK(manifest.covered_flops > 0);
  CHECK(manifest.covered_flops <= 22100);
  CHECK(manifest.storage_bytes > 0);
  CHECK(manifest.iterations_per_class == 100);
  CHECK(manifest.seed == 20261002);
  CHECK(manifest.player_count == 2);
  CHECK(manifest.big_blind == 2);
  CHECK(manifest.stack == 4);
  CHECK(manifest.contribution == 2);
  CHECK(manifest.card_id == bs::abstraction::suit_canonicalization_id());
  CHECK(fs::exists(dir / "manifest.json"));

  // No checkpoint files remain; only immutable published policies.
  for (const auto& entry : manifest.classes) {
    const fs::path artifact_path = dir / entry.artifact_name;
    CHECK(fs::exists(artifact_path));
    const fs::path checkpoint = dir / (artifact_path.stem().string() + "-checkpoint.db");
    CHECK(!fs::exists(checkpoint));

    const bs::artifacts::ArtifactProbe probe = bs::artifacts::probe_artifact(artifact_path);
    // A v3 artifact carries the suit-canonicalization declaration.
    CHECK(probe.manifest.card_abstraction.has_value());
    CHECK(probe.manifest.card_abstraction.value() == bs::abstraction::suit_canonicalization_id());
    // The stored board is the canonical class representative.
    CHECK(probe.game_def.has_value());
    CHECK(probe.game_def->board_size == 3);
    const std::array<int, 3> stored{probe.game_def->board[0], probe.game_def->board[1],
                                    probe.game_def->board[2]};
    CHECK(bs::abstraction::canonicalize(stored).board == stored);
    CHECK(stored == entry.canonical_board);
    // The manifest's stored-row count matches the probe's aggregate.
    CHECK(probe.information_sets > 0);
    CHECK(entry.stored_rows == probe.information_sets);
    CHECK(entry.completed_iterations == 100);
    CHECK(entry.sha256_hex == probe.sha256_hex);
  }

  // Independently recompute storage: the manifest's byte total must equal the
  // sum of the on-disk artifact sizes (counted, not trusted from the builder).
  std::error_code ec;
  std::uintmax_t measured_bytes = 0;
  for (const auto& entry : manifest.classes) {
    measured_bytes += fs::file_size(dir / entry.artifact_name, ec);
    CHECK(!ec);
  }
  CHECK(measured_bytes == manifest.storage_bytes);

  // Independently recompute coverage for the two built classes.
  const auto classes = bs::flop_library::enumerate_classes();
  const std::size_t expected_covered =
      bs::flop_library::count_covered_flops({classes[0], classes[1]});
  CHECK(manifest.covered_flops == expected_covered);
  return true;
}

}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "bs-flop-library-test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  bool ok = true;
  ok = test_enumerate_classes() && ok;
  ok = test_declared_range() && ok;
  ok = test_coverage() && ok;
  ok = test_build_library(dir) && ok;

  fs::remove_all(dir, ec);
  if (!ok) {
    std::printf("flop_library: tests FAILED\n");
    return 1;
  }
  std::printf("flop_library: all tests passed\n");
  return 0;
}
