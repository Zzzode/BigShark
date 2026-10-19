// RFC 0026: bucket-deduplicated binary format round-trip tests.
//
// Packs synthetic class policies where multiple concrete holdings share one
// card bucket (the real artifact pattern), mmaps the BSFLIB02 file, and
// verifies every holding projects to its bucket row bitwise. Also covers a
// history-action node, miss semantics (absent node vs absent bucket), the
// packer's same-bucket identity rejection, and SHA-256 verification.
#include <algorithm>
#include <array>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/mmap_library.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/range.hpp>
#include <bs/seat_policy.hpp>
#include <bs/strategy_artifact.hpp>
#include <bs/unified_game.hpp>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "resident/resident_index.hpp"

namespace fs = std::filesystem;
using namespace bs;
using bs::resident::CompactRowView;
using bs::resident::ProbabilityLayout;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

constexpr std::array<int, 3> kBoard{0, 1, 2};  // 2c 2d 2h (trip flop)

solver::InformationKey make_root_key(std::size_t actor, std::array<int, 2> own) {
  solver::InformationKey key;
  key.push_back(actor);
  key.push_back(static_cast<std::uint64_t>(own[0]));
  key.push_back(static_cast<std::uint64_t>(own[1]));
  key.push_back(3);  // flop board size
  key.push_back(kBoard[0]);
  key.push_back(kBoard[1]);
  key.push_back(kBoard[2]);
  return key;
}

solver::InformationKey make_history_key(std::size_t actor, std::array<int, 2> own,
                                        std::span<const poker::PublicAction> path) {
  solver::InformationKey key = make_root_key(actor, own);
  for (const poker::PublicAction& e : path) {
    key.push_back(static_cast<std::uint64_t>(e.street));
    key.push_back(e.seat);
    key.push_back(static_cast<std::uint64_t>(e.action.type));
    key.push_back(e.action.target_total);
  }
  return key;
}

// Strip the two own-card words from an information key and emit the public
// compact key the mmap reader hashes: [seat][board_size][board][5 bytes per
// action].
std::vector<std::uint8_t> public_key_from_words(std::span<const std::uint64_t> words) {
  std::vector<std::uint8_t> out;
  const std::size_t board_size = static_cast<std::size_t>(words[3]);
  out.push_back(static_cast<std::uint8_t>(words[0]));
  out.push_back(static_cast<std::uint8_t>(board_size));
  for (std::size_t i = 0; i < board_size; ++i)
    out.push_back(static_cast<std::uint8_t>(words[4 + i]));
  for (std::size_t w = 4 + board_size; w + 3 < words.size(); w += 4) {
    out.push_back(static_cast<std::uint8_t>(words[w]));
    out.push_back(static_cast<std::uint8_t>(words[w + 1]));
    out.push_back(static_cast<std::uint8_t>(words[w + 2]));
    out.push_back(static_cast<std::uint8_t>(words[w + 3] & 0xffu));
    out.push_back(static_cast<std::uint8_t>((words[w + 3] >> 8) & 0xffu));
  }
  return out;
}

double defer_prob(const CompactRowView& row, std::size_t i) {
  switch (row.prob_layout) {
    case ProbabilityLayout::Codebook8:
      return row.codebook[row.probability_indices[i]];
    case ProbabilityLayout::Codebook16:
      return row.codebook[row.probability_indices_16[i]];
    case ProbabilityLayout::Doubles:
      return row.probabilities[i];
  }
  return 0.0;
}

bool overlaps_board(std::array<int, 2> h) {
  for (int b : kBoard)
    if (h[0] == b || h[1] == b)
      return true;
  return false;
}

solver::UnifiedGame make_game() {
  solver::UnifiedGame game;
  game.def.player_count = 2;
  game.def.button = 0;
  game.def.big_blind = 2;
  game.def.stacks[0] = 200;
  game.def.stacks[1] = 200;
  game.def.contributions[0] = 2;
  game.def.contributions[1] = 2;
  game.def.pot = 4;
  game.def.terminal = poker::TerminalDepth::Flop;
  game.sizes = abstraction::default_size_schedule();
  return game;
}

// Build rows: pick a spread of off-board holdings; assign a probability vector
// derived only from the bucket so same-bucket holdings stay identical.
struct Fixture {
  std::map<solver::InformationKey, solver::SeatPolicyRow> root_rows;
  std::map<solver::InformationKey, solver::SeatPolicyRow> history_rows;
  std::vector<std::array<int, 2>> holdings;
  std::map<std::uint16_t, std::array<double, 2>> root_vector_by_bucket;
};

Fixture make_fixture() {
  Fixture fx;
  const poker::Action check{poker::ActionType::Check, 0};
  const poker::Action bet{poker::ActionType::Bet, 100};
  const poker::Action fold{poker::ActionType::Fold, 0};
  const poker::Action call{poker::ActionType::Call, 0};

  // On a trip-deuces board these span quads (fourth deuce + kicker), a full
  // house (a pocket pair), and trips (two unpaired, non-deuce cards).
  const std::array<std::array<int, 2>, 6> picked{{
      {3, 4},    // 2s3c: quads (four deuces)
      {3, 10},   // 2s + kicker: quads
      {4, 5},    // pocket pair of treys over 222: full house
      {8, 9},    // pocket pair of fives: full house
      {4, 8},    // unpaired no-deuce: trips
      {10, 14},  // unpaired no-deuce: trips
  }};
  for (std::array<int, 2> h : picked)
    fx.holdings.push_back(h);

  for (std::array<int, 2> h : fx.holdings) {
    const std::uint32_t b32 = abstraction::card_bucket(solver::kNSeatCardKind, h, kBoard);
    const std::uint16_t bucket = static_cast<std::uint16_t>(b32);
    if (!fx.root_vector_by_bucket.count(bucket)) {
      const double p = 0.1 + 0.03 * static_cast<double>(bucket);
      fx.root_vector_by_bucket.emplace(bucket, std::array<double, 2>{p, 1.0 - p});
    }
    const auto& v = fx.root_vector_by_bucket.at(bucket);
    fx.root_rows[make_root_key(0, h)] = solver::SeatPolicyRow{
        .actions = {check, bet}, .probabilities = {v[0], v[1]}, .visits = 100};
    // Second node after one public action: fold/call menu, bucket-derived vec.
    const poker::PublicAction path_event{poker::Street::Flop, 1,
                                         poker::Action{poker::ActionType::Bet, 100}};
    fx.history_rows[make_history_key(0, h, std::span(&path_event, 1))] = solver::SeatPolicyRow{
        .actions = {fold, call}, .probabilities = {1.0 - v[0], v[0]}, .visits = 50};
  }
  return fx;
}

std::vector<solver::WeightedHand> make_range() {
  return {{{3, 4}, 1.0}, {{5, 6}, 1.0}, {{7, 8}, 0.5}};
}

bool test_pack_and_mmap(const fs::path& dir) {
  const fs::path bin_path = dir / "test_library.bin";

  Fixture fx = make_fixture();
  CHECK(fx.root_vector_by_bucket.size() >= 2);  // at least two categories
  auto range = make_range();
  solver::UnifiedGame game = make_game();

  // Two classes: class 0 carries root rows; class 1 carries the history node.
  resident::PackClassInput inputs[2];
  inputs[0].class_id = 0;
  inputs[0].canonical_board = kBoard;
  inputs[0].rows = &fx.root_rows;
  inputs[0].range = &range;
  inputs[0].card_abstraction_digest = 0x123456789abcdef0ULL;
  inputs[0].action_abstraction_digest = 0xfedcba9876543210ULL;
  inputs[1].class_id = 1;
  inputs[1].canonical_board = kBoard;
  inputs[1].rows = &fx.history_rows;
  inputs[1].range = &range;
  inputs[1].card_abstraction_digest = 0x123456789abcdef0ULL;
  inputs[1].action_abstraction_digest = 0xfedcba9876543210ULL;

  try {
    resident::pack_binary_library(bin_path, game, std::span(inputs, 2));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "pack threw: %s\n", e.what());
    return false;
  }
  CHECK(fs::exists(bin_path));
  CHECK(fs::exists(bin_path.string() + ".sha256"));

  resident::MmapLibrary lib;
  try {
    lib.open(bin_path);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "open threw: %s\n", e.what());
    return false;
  }

  CHECK(lib.class_count() == 2);
  CHECK(lib.canonical_board(0) == kBoard);
  // Class 0: one root node, one bucket row per distinct category, two menus.
  CHECK(lib.node_count(0) == 1);
  CHECK(lib.bucket_row_count(0) == fx.root_vector_by_bucket.size());
  CHECK(lib.menu_action_count(0) == 2);
  CHECK(lib.card_abstraction_digest() == 0x123456789abcdef0ULL);
  CHECK(lib.action_abstraction_digest() == 0xfedcba9876543210ULL);
  CHECK(lib.card_bucket_kind() ==
        static_cast<std::uint8_t>(static_cast<std::underlying_type_t<abstraction::CardBucketKind>>(
            solver::kNSeatCardKind)));

  // Every holding projects to its bucket row; same-bucket holdings agree.
  const auto root_public = public_key_from_words(std::span<const std::uint64_t>(
      make_root_key(0, fx.holdings[0]).data(), make_root_key(0, fx.holdings[0]).size()));
  for (std::array<int, 2> h : fx.holdings) {
    const std::uint16_t bucket =
        static_cast<std::uint16_t>(abstraction::card_bucket(solver::kNSeatCardKind, h, kBoard));
    CompactRowView view;
    CHECK(lib.find_bucket(0, root_public, bucket, view));
    CHECK(view.count == 2);
    const auto& expected = fx.root_vector_by_bucket.at(bucket);
    CHECK(defer_prob(view, 0) == expected[0]);
    CHECK(defer_prob(view, 1) == expected[1]);
  }

  // History node round-trip (public key carries one 5-byte action).
  {
    const poker::PublicAction path_event{poker::Street::Flop, 1,
                                         poker::Action{poker::ActionType::Bet, 100}};
    const auto words = make_history_key(0, fx.holdings[0], std::span(&path_event, 1));
    const auto pub =
        public_key_from_words(std::span<const std::uint64_t>(words.data(), words.size()));
    const std::uint16_t bucket = static_cast<std::uint16_t>(
        abstraction::card_bucket(solver::kNSeatCardKind, fx.holdings[0], kBoard));
    CompactRowView view;
    CHECK(lib.find_bucket(1, pub, bucket, view));
    CHECK(view.count == 2);
    CHECK(static_cast<std::uint8_t>(view.actions[0].type) ==
          static_cast<std::uint8_t>(poker::ActionType::Fold));
    CHECK(static_cast<std::uint8_t>(view.actions[1].type) ==
          static_cast<std::uint8_t>(poker::ActionType::Call));
  }

  // Miss: unknown class.
  {
    CompactRowView view;
    CHECK(!lib.find_bucket(99, root_public, 1, view));
  }
  // Miss: present node, bucket never trained (choose an absent category).
  {
    std::uint16_t absent = 1;
    while (fx.root_vector_by_bucket.count(absent))
      ++absent;
    CompactRowView view;
    CHECK(!lib.find_bucket(0, root_public, absent, view));
  }
  // Miss: unknown public node within a known class.
  {
    const std::vector<std::uint8_t> bogus(root_public.size(), 0xff);
    CompactRowView view;
    CHECK(!lib.find_bucket(0, bogus, 1, view));
  }

  // Range round-trip.
  const auto& stored = lib.class_range(0);
  CHECK(stored.size() == range.size());
  for (std::size_t i = 0; i < range.size(); ++i) {
    CHECK(stored[i].cards == range[i].cards);
    CHECK(stored[i].weight == range[i].weight);
  }
  return g_failures == 0;
}

bool test_pack_rejects_same_bucket_conflict(const fs::path& dir) {
  // Two holdings in the SAME bucket with DIFFERENT vectors must fail loudly:
  // the packer must never merge distinct strategies under one bucket.
  const fs::path bin_path = dir / "test_conflict.bin";
  // Find two off-board holdings sharing a bucket.
  std::map<std::uint16_t, std::array<int, 2>> first;
  std::array<int, 2> a{}, b{};
  std::uint16_t shared = 0;
  bool found = false;
  for (int c0 = 3; c0 < 51 && !found; ++c0)
    for (int c1 = c0 + 1; c1 < 52 && !found; ++c1) {
      std::array<int, 2> h{c0, c1};
      if (overlaps_board(h))
        continue;
      const std::uint16_t bucket =
          static_cast<std::uint16_t>(abstraction::card_bucket(solver::kNSeatCardKind, h, kBoard));
      if (first.count(bucket)) {
        a = first.at(bucket);
        b = h;
        shared = bucket;
        found = true;
      } else {
        first.emplace(bucket, h);
      }
    }
  CHECK(found);
  (void)shared;

  const poker::Action check{poker::ActionType::Check, 0};
  const poker::Action bet{poker::ActionType::Bet, 100};
  std::map<solver::InformationKey, solver::SeatPolicyRow> rows;
  rows[make_root_key(0, a)] =
      solver::SeatPolicyRow{.actions = {check, bet}, .probabilities = {0.2, 0.8}, .visits = 1};
  rows[make_root_key(0, b)] =
      solver::SeatPolicyRow{.actions = {check, bet}, .probabilities = {0.7, 0.3}, .visits = 1};
  resident::PackClassInput input;
  input.class_id = 0;
  input.canonical_board = kBoard;
  input.rows = &rows;
  solver::UnifiedGame game = make_game();

  bool threw = false;
  try {
    resident::pack_binary_library(bin_path, game, std::span(&input, 1));
  } catch (const std::exception& e) {
    threw = std::string(e.what()).find("same bucket") != std::string::npos;
  }
  CHECK(threw);
  CHECK(!fs::exists(bin_path));
  return g_failures == 0;
}

bool test_sha256_verification(const fs::path& dir) {
  const fs::path bin_path = dir / "test_sha.bin";
  Fixture fx = make_fixture();
  auto range = make_range();
  solver::UnifiedGame game = make_game();
  resident::PackClassInput input;
  input.class_id = 0;
  input.canonical_board = kBoard;
  input.rows = &fx.root_rows;
  input.range = &range;
  try {
    resident::pack_binary_library(bin_path, game, std::span(&input, 1));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "pack threw: %s\n", e.what());
    return false;
  }
  const fs::path sha_path = bin_path.string() + ".sha256";
  std::string sha_content;
  {
    std::ifstream f(sha_path, std::ios::binary);
    sha_content.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  if (!sha_content.empty()) {
    sha_content[0] = (sha_content[0] == '0') ? '1' : '0';
    std::ofstream f(sha_path, std::ios::binary | std::ios::trunc);
    f << sha_content;
  }
  resident::MmapLibrary lib;
  bool threw = false;
  try {
    lib.open(bin_path);
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
  return g_failures == 0;
}

// RFC 0027: range canonicalization. The serializer sorts the declared range by
// comboIndex, so a shuffled input must produce the same section bytes as the
// canonical order, and a duplicate combo is rejected. This is the property
// that makes a direct build match the SQLite packer for non-full ranges.
bool test_range_canonicalization(const fs::path& dir) {
  Fixture fx = make_fixture();
  auto shuffled = make_range();
  std::reverse(shuffled.begin(), shuffled.end());
  auto sorted = make_range();
  std::sort(sorted.begin(), sorted.end(),
            [](const solver::WeightedHand& a, const solver::WeightedHand& b) {
              return comboIndex(a.cards[0], a.cards[1]) < comboIndex(b.cards[0], b.cards[1]);
            });

  resident::PackClassInput a;
  a.class_id = 0;
  a.canonical_board = kBoard;
  a.rows = &fx.root_rows;
  a.range = &shuffled;
  resident::PackClassInput b = a;
  b.range = &sorted;

  const std::vector<std::uint8_t> section_a =
      resident::serialize_class_section(a, resident::ProbabilityLayout::Codebook16);
  const std::vector<std::uint8_t> section_b =
      resident::serialize_class_section(b, resident::ProbabilityLayout::Codebook16);
  CHECK(section_a == section_b);

  // A duplicate combo cannot be represented and must be rejected.
  auto dup = sorted;
  dup.push_back(sorted.front());
  resident::PackClassInput c = a;
  c.range = &dup;
  bool threw = false;
  try {
    (void)resident::serialize_class_section(c, resident::ProbabilityLayout::Codebook16);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return true;
}

// RFC 0027: the in-memory assembler and the file-backed assembler produce the
// same bytes from the same sections, and the section is exactly its class.
bool test_assemble_matches_pack(const fs::path& dir) {
  Fixture fx = make_fixture();
  auto range = make_range();
  solver::UnifiedGame game = make_game();
  std::vector<resident::PackClassInput> inputs;
  for (std::uint32_t id = 0; id < 2; ++id) {
    resident::PackClassInput input;
    input.class_id = id;
    input.canonical_board = kBoard;
    input.rows = id == 0 ? &fx.root_rows : &fx.history_rows;
    input.range = &range;
    inputs.push_back(input);
  }

  const fs::path packed = dir / "assemble_packed.bin";
  resident::pack_binary_library(packed, game, inputs);

  std::vector<resident::ClassSectionInput> sections;
  std::vector<std::vector<std::uint8_t>> storage;
  storage.reserve(inputs.size());
  for (const auto& input : inputs)
    storage.push_back(
        resident::serialize_class_section(input, resident::ProbabilityLayout::Codebook16));
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    resident::ClassSectionInput section;
    section.class_id = inputs[i].class_id;
    section.bytes = storage[i];
    sections.push_back(section);
  }
  const fs::path assembled = dir / "assemble_direct.bin";
  resident::LibraryIdentity identity;
  resident::assemble_binary_library(assembled, game, identity,
                                    resident::ProbabilityLayout::Codebook16, sections);
  CHECK(artifacts::sha256_file_hex(packed) == artifacts::sha256_file_hex(assembled));
  return true;
}

// RFC 0027: a section whose byte length does not match the layout's counters is
// rejected before any output byte is written.
bool test_assembly_rejects_layout_mismatch(const fs::path& dir) {
  Fixture fx = make_fixture();
  auto range = make_range();
  solver::UnifiedGame game = make_game();
  resident::PackClassInput input;
  input.class_id = 0;
  input.canonical_board = kBoard;
  input.rows = &fx.root_rows;
  input.range = &range;
  const std::vector<std::uint8_t> codebook16 =
      resident::serialize_class_section(input, resident::ProbabilityLayout::Codebook16);

  std::vector<resident::ClassSectionInput> sections;
  resident::ClassSectionInput section;
  section.class_id = 0;
  section.bytes = codebook16;
  sections.push_back(section);
  const fs::path out = dir / "layout_mismatch.bin";
  resident::LibraryIdentity identity;
  bool threw = false;
  try {
    resident::assemble_binary_library(out, game, identity, resident::ProbabilityLayout::Doubles,
                                      sections);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
  CHECK(!fs::exists(out));
  CHECK(!fs::exists(out.string() + ".tmp"));
  return true;
}

}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "bs_test_mmap_library_v2";
  fs::remove_all(dir);
  fs::create_directories(dir);

  bool ok = true;
  ok = test_pack_and_mmap(dir) && ok;
  ok = test_pack_rejects_same_bucket_conflict(dir) && ok;
  ok = test_sha256_verification(dir) && ok;
  ok = test_range_canonicalization(dir) && ok;
  ok = test_assemble_matches_pack(dir) && ok;
  ok = test_assembly_rejects_layout_mismatch(dir) && ok;

  fs::remove_all(dir);
  if (!ok || g_failures > 0) {
    std::fprintf(stderr, "mmap_library: %d failures\n", g_failures);
    return 1;
  }
  std::printf("mmap_library: all tests passed\n");
  return 0;
}
