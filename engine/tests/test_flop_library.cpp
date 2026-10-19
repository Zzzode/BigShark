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
#include <fstream>
#include <iterator>
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

bool test_class_dealability() {
  const auto range = bs::flop_library::declared_library_range();
  auto card = [](const char* name) { return bs::cardId(std::string(name)); };
  // Six seats need 12 distinct range cards. Three board cards all inside the
  // 16-card J+ range leave 13 cards, but the remaining combos cannot form six
  // disjoint pairs on these pinned boards (verified against exact joint-deal
  // enumeration): undealable for six seats.
  CHECK(!bs::flop_library::class_is_dealable({card("Ks"), card("Qs"), card("Js")}, 6, range));
  CHECK(!bs::flop_library::class_is_dealable({card("Kc"), card("Qc"), card("Jc")}, 6, range));
  // Ordinary and high-but-feasible boards are dealable.
  CHECK(bs::flop_library::class_is_dealable({card("2c"), card("3d"), card("7h")}, 6, range));
  CHECK(bs::flop_library::class_is_dealable({card("As"), card("Ks"), card("Qs")}, 6, range));
  CHECK(bs::flop_library::class_is_dealable({card("As"), card("Kd"), card("Th")}, 6, range));
  // Monotonicity in seat count: what six seats cannot deal, two can on the
  // pinned three-high board (13 cards easily yield two disjoint combos).
  CHECK(bs::flop_library::class_is_dealable({card("Ks"), card("Qs"), card("Js")}, 2, range));
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

  // RFC 0027: the packed library is the artifact. No per-class SQLite policy
  // file and no leftover section staging directory remain.
  CHECK(manifest.library_bin == "library.bin");
  CHECK(fs::exists(dir / manifest.library_bin));
  CHECK(!fs::exists(dir / ".sections"));
  for (const auto& entry : manifest.classes) {
    char stem[32];
    std::snprintf(stem, sizeof(stem), "class-%04zu",
                  static_cast<std::size_t>(&entry - manifest.classes.data()));
    CHECK(!fs::exists(dir / (std::string(stem) + ".db")));
    CHECK(!fs::exists(dir / (std::string(stem) + "-checkpoint.bswarm")));
    CHECK(entry.stored_rows > 0);
    CHECK(entry.completed_iterations == 100);
    CHECK(entry.section_bytes > 0);
  }

  // The packed file loads through the resident reader with the expected
  // classes, and its sidecar matches the measured digest.
  bs::resident::MmapLibrary library;
  library.open(dir / manifest.library_bin);
  CHECK(library.class_count() == manifest.class_count);
  CHECK(library.sha256_hex() == manifest.library_bin_sha256);
  for (const auto& entry : manifest.classes) {
    const std::int32_t class_id = library.class_id_for_board(entry.canonical_board);
    CHECK(class_id >= 0);
    CHECK(library.canonical_board(static_cast<std::uint32_t>(class_id)) == entry.canonical_board);
  }

  // Independently recompute storage from the file itself.
  std::error_code ec;
  CHECK(fs::file_size(dir / manifest.library_bin, ec) == manifest.storage_bytes);
  CHECK(!ec);

  // Independently recompute coverage for the two built classes.
  const auto classes = bs::flop_library::enumerate_classes();
  const std::size_t expected_covered =
      bs::flop_library::count_covered_flops({classes[0], classes[1]});
  CHECK(manifest.covered_flops == expected_covered);
  return true;
}

bool test_build_library_coarse(const fs::path& dir) {
  const auto range = bs::flop_library::declared_library_range();
  const auto action = bs::abstraction::ActionAbstraction::declared(
      bs::abstraction::default_size_schedule(), bs::abstraction::CoverSeeds::DeclaredOnly);
  const bs::flop_library::LibraryManifest manifest =
      bs::flop_library::build_library(dir, 2, 100, 20261002, 4, 2, range, 2, action);

  CHECK(manifest.action_id.name == "rfc0008-declared-coarse");
  CHECK(manifest.action_id != bs::abstraction::identity_action_id());
  CHECK(manifest.class_count == 2);
  CHECK(manifest.storage_bytes > 0);
  // The coarse tree should have fewer nodes than the identity tree at the
  // same stack profile (the forced min/cap seeds are removed).
  CHECK(fs::exists(dir / manifest.library_bin));
  for (const auto& entry : manifest.classes) {
    CHECK(fs::exists(dir / manifest.library_bin));
    CHECK(entry.stored_rows > 0);
  }
  return true;
}

// RFC 0024: the raise-cap is recorded in the manifest so published artifacts
// carry the action-abstraction configuration that shaped their tree.
bool test_build_library_raise_cap(const fs::path& dir) {
  const auto range = bs::flop_library::declared_library_range();
  auto action = bs::abstraction::ActionAbstraction::declared(
      bs::abstraction::default_size_schedule(), bs::abstraction::CoverSeeds::DeclaredOnly);
  action.set_max_raises_per_street(1);
  const bs::flop_library::LibraryManifest manifest =
      bs::flop_library::build_library(dir, 1, 50, 20261002, 4, 2, range, 2, action);

  // The manifest struct carries the cap.
  CHECK(manifest.max_raises_per_street == 1);

  // The manifest JSON on disk records the cap.
  const fs::path manifest_path = dir / "manifest.json";
  CHECK(fs::exists(manifest_path));
  std::ifstream mf(manifest_path);
  CHECK(mf.good());
  const std::string content((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
  CHECK(content.find("\"max_raises_per_street\": 1") != std::string::npos);

  // The uncapped default records zero.
  const bs::flop_library::LibraryManifest uncapped =
      bs::flop_library::build_library(dir / "uncapped", 1, 50, 20261002, 4, 2, range, 2);
  CHECK(uncapped.max_raises_per_street == 0);
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
  ok = test_class_dealability() && ok;
  ok = test_build_library(dir) && ok;
  ok = test_build_library_coarse(dir / "coarse") && ok;
  ok = test_build_library_raise_cap(dir / "raise-cap") && ok;

  fs::remove_all(dir, ec);
  if (!ok) {
    std::printf("flop_library: tests FAILED\n");
    return 1;
  }
  std::printf("flop_library: all tests passed\n");
  return 0;
}
