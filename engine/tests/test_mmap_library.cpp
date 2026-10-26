// RFC 0026/RFC 0035: multi-root bucket-deduplicated binary format tests.
//
// Packs synthetic class policies where multiple concrete holdings share one
// card bucket (the real artifact pattern), mmaps the BSFLIB03 file, and
// verifies every holding projects to its (root, class) bucket row bitwise.
// Covers the root directory, two-root addressing, the full RFC 0030 action
// identity round-trip, miss semantics, every mount-time refusal (bad/old
// magic, truncation, duplicate ledgers, out-of-file offsets, misindexed
// sections, bad class-header alignment), the packer's same-bucket identity
// rejection, and SHA-256 verification.
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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "resident/resident_index.hpp"

namespace fs = std::filesystem;
using namespace bs;
using bs::resident::CompactRowView;
using bs::resident::ProbabilityLayout;
using bs::resident::RootDirectoryEntry;
using bs::resident::RootLineTag;
using bs::resident::RootRangeProfile;

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

solver::UnifiedGame make_game(std::uint8_t max_raises = 1) {
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
  // RFC 0035: the header carries the full identity, including the cap token.
  game.action_id = abstraction::ActionAbstraction::identity(max_raises).id();
  return game;
}

// The single limped root (2 seats, 198 behind, 2 in, pot 4).
RootDirectoryEntry make_root(std::uint32_t id, RootLineTag tag = RootLineTag::Limped,
                             std::uint64_t stack0 = 198, std::uint64_t stack1 = 198,
                             std::uint64_t contrib0 = 2, std::uint64_t contrib1 = 2,
                             std::uint8_t folded0 = 0, std::uint8_t folded1 = 0) {
  RootDirectoryEntry root;
  root.root_id = id;
  root.line_tag = tag;
  root.range_profile = RootRangeProfile::LiveUniform;
  root.stacks = {stack0, stack1};
  root.contributions = {contrib0, contrib1};
  root.folded = {folded0, folded1};
  root.pot = contrib0 + contrib1;
  root.live_count = static_cast<std::uint8_t>(2 - (folded0 != 0) - (folded1 != 0));
  return root;
}

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

  const std::array<std::array<int, 2>, 6> picked{{
      {3, 4},
      {3, 10},
      {4, 5},
      {8, 9},
      {4, 8},
      {10, 14},
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

// Build two PackClassInputs for root 0 (class 0 = root rows, class 1 = history
// node). Shared digests give the header a real strict action identity.
std::array<resident::PackClassInput, 2> make_inputs(const Fixture& fx,
                                                    const std::vector<solver::WeightedHand>& range,
                                                    std::uint32_t root_id = 0) {
  std::array<resident::PackClassInput, 2> inputs{};
  inputs[0].root_id = root_id;
  inputs[0].class_id = 0;
  inputs[0].canonical_board = kBoard;
  inputs[0].rows = &fx.root_rows;
  inputs[0].range = &range;
  inputs[0].card_abstraction_digest = 0x123456789abcdef0ULL;
  inputs[0].action_abstraction_digest = 0;  // replaced below with the real digest
  inputs[0].card_abstraction_digest = 0x123456789abcdef0ULL;
  inputs[1].root_id = root_id;
  inputs[1].class_id = 1;
  inputs[1].canonical_board = kBoard;
  inputs[1].rows = &fx.history_rows;
  inputs[1].range = &range;
  inputs[1].card_abstraction_digest = 0x123456789abcdef0ULL;
  inputs[1].action_abstraction_digest = 0;
  return inputs;
}

// Round-trip on one root: packing, mmap, per-(root,class) bucket projection.
bool test_pack_and_mmap(const fs::path& dir) {
  const fs::path bin_path = dir / "test_library.bin";

  Fixture fx = make_fixture();
  CHECK(fx.root_vector_by_bucket.size() >= 2);
  auto range = make_range();
  solver::UnifiedGame game = make_game();
  auto inputs = make_inputs(fx, range);
  for (auto& input : inputs) {
    input.action_abstraction_digest = game.action_id.digest;
    input.card_abstraction_digest = 0x123456789abcdef0ULL;
  }

  std::vector<RootDirectoryEntry> roots{make_root(0)};
  try {
    resident::pack_binary_library(bin_path, game, roots, std::span(inputs.data(), 2));
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

  CHECK(lib.root_count() == 1);
  CHECK(lib.root(0).pot == 4);
  CHECK(lib.root(0).live_count == 2);
  CHECK(lib.root(0).range_profile == RootRangeProfile::LiveUniform);
  CHECK(lib.root_class_ids(0).size() == 2);
  CHECK(lib.has_class(0, 0));
  CHECK(lib.has_class(0, 1));
  CHECK(!lib.has_class(0, 2));
  CHECK(!lib.has_class(1, 0));
  CHECK(lib.node_count(0, 0) == 1);
  CHECK(lib.bucket_row_count(0, 0) == fx.root_vector_by_bucket.size());
  CHECK(lib.menu_action_count(0, 0) == 2);
  CHECK(lib.card_abstraction_digest() == 0x123456789abcdef0ULL);
  CHECK(lib.action_abstraction_digest() == game.action_id.digest);
  // RFC 0035: the reader reconstructs the full action identity and cap.
  CHECK(lib.action_identity() == game.action_id);
  CHECK(abstraction::parse_max_raises_token(lib.action_identity().parameters) == 1);
  CHECK(lib.game().action_id == game.action_id);
  CHECK(lib.card_bucket_kind() ==
        static_cast<std::uint8_t>(static_cast<std::underlying_type_t<abstraction::CardBucketKind>>(
            solver::kNSeatCardKind)));

  // Single-root tooling accessors work on a one-root file.
  CHECK(lib.class_count() == 2);
  CHECK(lib.class_id_at(1) == 1);
  CHECK(lib.class_id_for_root_board(0, kBoard) >= 0);

  const auto root_public = public_key_from_words(std::span<const std::uint64_t>(
      make_root_key(0, fx.holdings[0]).data(), make_root_key(0, fx.holdings[0]).size()));
  for (std::array<int, 2> h : fx.holdings) {
    const std::uint16_t bucket =
        static_cast<std::uint16_t>(abstraction::card_bucket(solver::kNSeatCardKind, h, kBoard));
    CompactRowView view;
    CHECK(lib.find_bucket(0, 0, root_public, bucket, view));
    CHECK(view.count == 2);
    const auto& expected = fx.root_vector_by_bucket.at(bucket);
    CHECK(defer_prob(view, 0) == expected[0]);
    CHECK(defer_prob(view, 1) == expected[1]);
  }

  {
    const poker::PublicAction path_event{poker::Street::Flop, 1,
                                         poker::Action{poker::ActionType::Bet, 100}};
    const auto words = make_history_key(0, fx.holdings[0], std::span(&path_event, 1));
    const auto pub =
        public_key_from_words(std::span<const std::uint64_t>(words.data(), words.size()));
    const std::uint16_t bucket = static_cast<std::uint16_t>(
        abstraction::card_bucket(solver::kNSeatCardKind, fx.holdings[0], kBoard));
    CompactRowView view;
    CHECK(lib.find_bucket(0, 1, pub, bucket, view));
    CHECK(view.count == 2);
    CHECK(static_cast<std::uint8_t>(view.actions[0].type) ==
          static_cast<std::uint8_t>(poker::ActionType::Fold));
    CHECK(static_cast<std::uint8_t>(view.actions[1].type) ==
          static_cast<std::uint8_t>(poker::ActionType::Call));
  }

  // Miss: absent (root, class) pair.
  {
    CompactRowView view;
    CHECK(!lib.find_bucket(1, 0, root_public, 1, view));
    CHECK(!lib.find_bucket(0, 99, root_public, 1, view));
  }
  {
    std::uint16_t absent = 1;
    while (fx.root_vector_by_bucket.count(absent))
      ++absent;
    CompactRowView view;
    CHECK(!lib.find_bucket(0, 0, root_public, absent, view));
  }
  {
    const std::vector<std::uint8_t> bogus(root_public.size(), 0xff);
    CompactRowView view;
    CHECK(!lib.find_bucket(0, 0, bogus, 1, view));
  }

  const auto& stored = lib.class_range(0, 0);
  CHECK(stored.size() == range.size());
  for (std::size_t i = 0; i < range.size(); ++i) {
    CHECK(stored[i].cards == range[i].cards);
    CHECK(stored[i].weight == range[i].weight);
  }

  // Positional enumeration agrees with the keyed lookup.
  for (std::uint32_t cid = 0; cid < 2; ++cid) {
    const std::span<const std::uint8_t> pub =
        cid == 0 ? root_public : std::span<const std::uint8_t>();
    std::size_t enumerated = 0;
    for (std::size_t n = 0; n < lib.node_count(0, cid); ++n) {
      const std::size_t rows = lib.node_bucket_count_at(0, cid, n);
      for (std::size_t r = 0; r < rows; ++r) {
        const std::uint16_t bucket = lib.node_bucket_id_at(0, cid, n, r);
        CompactRowView enumerated_view;
        lib.node_bucket_row_at(0, cid, n, r, enumerated_view);
        CHECK(enumerated_view.count == 2);
        double sum = 0.0;
        for (std::uint16_t a = 0; a < enumerated_view.count; ++a) {
          const double p = defer_prob(enumerated_view, a);
          CHECK(std::isfinite(p));
          sum += p;
        }
        CHECK(std::fabs(sum - 1.0) < 1e-9);
        if (cid == 0) {
          CompactRowView keyed_view;
          CHECK(lib.find_bucket(0, cid, pub, bucket, keyed_view));
          for (std::uint16_t a = 0; a < 2; ++a)
            CHECK(defer_prob(enumerated_view, a) == defer_prob(keyed_view, a));
        }
        ++enumerated;
      }
    }
    CHECK(enumerated == lib.bucket_row_count(0, cid));
  }

  // The single-root accessors throw on out-of-range enumeration.
  {
    CompactRowView view;
    bool threw = false;
    try {
      lib.node_bucket_row_at(0, 99, 0, view);
    } catch (const std::out_of_range&) {
      threw = true;
    }
    CHECK(threw);
  }
  return g_failures == 0;
}

// RFC 0035: two distinct roots with one present class each. Class 0 is trained
// for root 0 only, class 1 for root 1 only, so the dense rows differ.
bool test_two_root_library(const fs::path& dir) {
  const fs::path bin_path = dir / "two_root.bin";
  Fixture fx = make_fixture();
  auto range = make_range();
  solver::UnifiedGame game = make_game();
  std::vector<RootDirectoryEntry> roots{make_root(0),
                                        make_root(1, RootLineTag::SingleRaisedPot, 194, 194, 6, 6)};
  resident::PackClassInput inputs[2];
  inputs[0].root_id = 0;
  inputs[0].class_id = 0;
  inputs[0].canonical_board = kBoard;
  inputs[0].rows = &fx.root_rows;
  inputs[0].range = &range;
  inputs[0].action_abstraction_digest = game.action_id.digest;
  inputs[0].card_abstraction_digest = 0x123456789abcdef0ULL;
  inputs[1].root_id = 1;
  inputs[1].class_id = 1;
  inputs[1].canonical_board = kBoard;
  inputs[1].rows = &fx.history_rows;
  inputs[1].range = &range;
  inputs[1].action_abstraction_digest = game.action_id.digest;
  inputs[1].card_abstraction_digest = 0x123456789abcdef0ULL;
  resident::pack_binary_library(bin_path, game, roots, std::span(inputs, 2));

  resident::MmapLibrary lib;
  lib.open(bin_path);
  CHECK(lib.root_count() == 2);
  CHECK(lib.root(0).pot == 4);
  CHECK(lib.root(1).pot == 12);
  CHECK(lib.root(1).line_tag == RootLineTag::SingleRaisedPot);
  CHECK(lib.root_class_ids(0).size() == 1);
  CHECK(lib.root_class_ids(0)[0] == 0);
  CHECK(lib.root_class_ids(1).size() == 1);
  CHECK(lib.root_class_ids(1)[0] == 1);
  CHECK(lib.has_class(0, 0));
  CHECK(lib.has_class(1, 1));
  CHECK(!lib.has_class(0, 1));
  CHECK(!lib.has_class(1, 0));

  const auto root_public = public_key_from_words(std::span<const std::uint64_t>(
      make_root_key(0, fx.holdings[0]).data(), make_root_key(0, fx.holdings[0]).size()));
  CompactRowView view;
  CHECK(lib.find_bucket(0, 0, root_public,
                        static_cast<std::uint16_t>(abstraction::card_bucket(
                            solver::kNSeatCardKind, fx.holdings[0], kBoard)),
                        view));
  CHECK(!lib.find_bucket(1, 0, root_public, 1, view));

  // Single-root tooling accessors must refuse a multi-root file.
  bool threw = false;
  try {
    (void)lib.class_count();
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return g_failures == 0;
}

bool test_pack_rejects_same_bucket_conflict(const fs::path& dir) {
  const fs::path bin_path = dir / "test_conflict.bin";
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
  input.root_id = 0;
  input.class_id = 0;
  input.canonical_board = kBoard;
  input.rows = &rows;
  solver::UnifiedGame game = make_game();
  std::vector<RootDirectoryEntry> roots{make_root(0)};

  bool threw = false;
  try {
    resident::pack_binary_library(bin_path, game, roots, std::span(&input, 1));
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
  input.action_abstraction_digest = game.action_id.digest;
  input.card_abstraction_digest = 0x123456789abcdef0ULL;
  std::vector<RootDirectoryEntry> roots{make_root(0)};
  resident::pack_binary_library(bin_path, game, roots, std::span(&input, 1));
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

bool test_range_canonicalization(const fs::path& dir) {
  (void)dir;
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

bool test_assemble_matches_pack(const fs::path& dir) {
  Fixture fx = make_fixture();
  auto range = make_range();
  solver::UnifiedGame game = make_game();
  std::vector<resident::PackClassInput> inputs;
  for (std::uint32_t id = 0; id < 2; ++id) {
    resident::PackClassInput input;
    input.root_id = 0;
    input.class_id = id;
    input.canonical_board = kBoard;
    input.rows = id == 0 ? &fx.root_rows : &fx.history_rows;
    input.range = &range;
    input.action_abstraction_digest = game.action_id.digest;
    input.card_abstraction_digest = 0x123456789abcdef0ULL;
    inputs.push_back(input);
  }
  std::vector<RootDirectoryEntry> roots{make_root(0)};

  const fs::path packed = dir / "assemble_packed.bin";
  resident::pack_binary_library(packed, game, roots, inputs);

  std::vector<resident::RootClassSection> sections;
  std::vector<std::vector<std::uint8_t>> storage;
  storage.reserve(inputs.size());
  for (const auto& input : inputs)
    storage.push_back(
        resident::serialize_class_section(input, resident::ProbabilityLayout::Codebook16));
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    resident::RootClassSection section;
    section.root_id = inputs[i].root_id;
    section.class_id = inputs[i].class_id;
    section.bytes = storage[i];
    sections.push_back(section);
  }
  const fs::path assembled = dir / "assemble_direct.bin";
  resident::LibraryIdentity identity;
  identity.action_abstraction_digest = game.action_id.digest;
  identity.card_abstraction_digest = 0x123456789abcdef0ULL;
  resident::assemble_binary_library(assembled, game, identity,
                                    resident::ProbabilityLayout::Codebook16, roots, sections);
  CHECK(artifacts::sha256_file_hex(packed) == artifacts::sha256_file_hex(assembled));
  return true;
}

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

  std::vector<resident::RootClassSection> sections;
  resident::RootClassSection section;
  section.class_id = 0;
  section.bytes = codebook16;
  sections.push_back(section);
  std::vector<RootDirectoryEntry> roots{make_root(0)};
  const fs::path out = dir / "layout_mismatch.bin";
  resident::LibraryIdentity identity;
  bool threw = false;
  try {
    resident::assemble_binary_library(out, game, identity, resident::ProbabilityLayout::Doubles,
                                      roots, sections);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
  CHECK(!fs::exists(out));
  CHECK(!fs::exists(out.string() + ".tmp"));
  return true;
}

// --- RFC 0035 mount-time refusal cases --------------------------------------

// Read the file bytes.
std::vector<std::uint8_t> read_bytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_bytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

bool opens_ok(const fs::path& path) {
  resident::MmapLibrary lib;
  try {
    lib.open(path, /*verify_sha256=*/false);
  } catch (const std::exception&) {
    return false;
  }
  return true;
}

// Build a valid one-root, two-class library plus its sidecar for corruption
// tests. Returns the bytes and writes a matching (or intentionally absent)
// sidecar.
struct LibraryFile {
  fs::path path;
  std::vector<std::uint8_t> bytes;
};

LibraryFile make_valid_library(const fs::path& dir, const std::string& name) {
  Fixture fx = make_fixture();
  auto range = make_range();
  solver::UnifiedGame game = make_game();
  auto inputs = make_inputs(fx, range);
  for (auto& input : inputs) {
    input.action_abstraction_digest = game.action_id.digest;
    input.card_abstraction_digest = 0x123456789abcdef0ULL;
  }
  std::vector<RootDirectoryEntry> roots{make_root(0)};
  const fs::path path = dir / name;
  resident::pack_binary_library(path, game, roots, std::span(inputs.data(), 2));
  return {path, read_bytes(path)};
}

bool expect_open_refused(const fs::path& path) {
  resident::MmapLibrary lib;
  bool threw = false;
  try {
    lib.open(path, /*verify_sha256=*/false);
  } catch (const std::exception&) {
    threw = true;
  }
  return threw;
}

bool test_old_magic_refused(const fs::path& dir) {
  // The BSFLIB02 magic is foreign: no dual reader exists.
  LibraryFile file = make_valid_library(dir, "old_magic.bin");
  auto bytes = file.bytes;
  const char old_magic[8] = {'B', 'S', 'F', 'L', 'I', 'B', '0', '2'};
  std::memcpy(bytes.data(), old_magic, 8);
  write_bytes(file.path, bytes);
  CHECK(expect_open_refused(file.path));

  // A completely foreign magic fails the same way.
  bytes[0] = 'X';
  write_bytes(file.path, bytes);
  CHECK(expect_open_refused(file.path));
  return true;
}

bool test_truncation_refused(const fs::path& dir) {
  LibraryFile file = make_valid_library(dir, "truncated.bin");

  // Truncated header (inside the first page).
  write_bytes(file.path, std::vector<std::uint8_t>(file.bytes.begin(), file.bytes.begin() + 128));
  CHECK(expect_open_refused(file.path));

  // Truncated inside the root directory (before the 4096 boundary).
  write_bytes(file.path, std::vector<std::uint8_t>(file.bytes.begin(), file.bytes.begin() + 4000));
  CHECK(expect_open_refused(file.path));

  // Truncated in the section region: the file keeps the header and index but
  // loses a page past the first section, which the exact length check must
  // catch.
  write_bytes(file.path,
              std::vector<std::uint8_t>(file.bytes.begin(), file.bytes.begin() + 4096 + 64));
  CHECK(expect_open_refused(file.path));
  return true;
}

bool test_duplicate_root_ledger_refused(const fs::path& dir) {
  // Two roots with identical ledgers cannot be written.
  Fixture fx = make_fixture();
  auto range = make_range();
  solver::UnifiedGame game = make_game();
  resident::PackClassInput inputs[2];
  inputs[0] = make_inputs(fx, range)[0];
  inputs[0].action_abstraction_digest = game.action_id.digest;
  inputs[1] = inputs[0];
  inputs[1].root_id = 1;
  std::vector<RootDirectoryEntry> roots{make_root(0), make_root(1)};
  const fs::path out = dir / "dup_ledger.bin";
  bool threw = false;
  try {
    resident::pack_binary_library(out, game, roots, std::span(inputs, 2));
  } catch (const std::exception& e) {
    threw = std::string(e.what()).find("duplicate") != std::string::npos;
  }
  CHECK(threw);
  CHECK(!fs::exists(out));

  // Hand-corrupt the on-disk directory of a valid one-root file so it claims
  // two roots with the same ledger. Simpler: build a real two-root file with
  // distinct ledgers, then copy root 0's ledger fields into root 1's record.
  resident::PackClassInput distinct[2];
  distinct[0] = inputs[0];
  distinct[1] = inputs[1];
  std::vector<RootDirectoryEntry> distinct_roots{
      make_root(0), make_root(1, RootLineTag::SingleRaisedPot, 194, 194, 6, 6)};
  const fs::path two_path = dir / "two_real.bin";
  resident::pack_binary_library(two_path, game, distinct_roots, std::span(distinct, 2));
  auto bytes = read_bytes(two_path);
  // Locate the directory: parse header lengths the same way the reader does.
  const auto u32 = [&](std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) |
           (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
  };
  const std::uint32_t name_len = u32(48);
  const std::uint32_t params_len = u32(56);
  const std::size_t kPrefix = 64;
  const std::size_t params_pos = (kPrefix + name_len + 7) & ~std::size_t{7};
  const std::size_t sizes_pos = (params_pos + params_len + 7) & ~std::size_t{7};
  std::size_t p = sizes_pos;
  std::size_t schedule = 0;
  for (std::size_t street = 0; street < 4; ++street) {
    const std::uint8_t bets = bytes[p + schedule];
    const std::uint8_t raises = bytes[p + schedule + 1];
    schedule += 2 + static_cast<std::size_t>(bets) * 16 + static_cast<std::size_t>(raises) * 16;
  }
  const std::size_t directory_pos = sizes_pos + schedule;
  const std::size_t root1 = directory_pos + 192;
  // Overwrite root 1's pot, stacks, contributions with root 0's values.
  for (std::size_t off : {std::size_t(8)})
    for (int b = 0; b < 8; ++b)
      bytes[root1 + off + b] = bytes[directory_pos + off + b];
  for (std::size_t off = 16; off < 16 + 160; ++off)
    bytes[root1 + off] = bytes[directory_pos + off];
  // Fix the SHA sidecar: structural validation runs with verification off.
  write_bytes(two_path, bytes);
  CHECK(expect_open_refused(two_path));
  return true;
}

bool test_index_offset_outside_file_refused(const fs::path& dir) {
  LibraryFile file = make_valid_library(dir, "bad_offset.bin");
  auto bytes = file.bytes;
  const auto u32 = [&](std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) |
           (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
  };
  const std::uint32_t name_len = u32(48);
  const std::uint32_t params_len = u32(56);
  const std::size_t kPrefix = 64;
  const std::size_t params_pos = (kPrefix + name_len + 7) & ~std::size_t{7};
  const std::size_t sizes_pos = (params_pos + params_len + 7) & ~std::size_t{7};
  std::size_t p = sizes_pos;
  std::size_t schedule = 0;
  for (std::size_t street = 0; street < 4; ++street) {
    const std::uint8_t bets = bytes[p + schedule];
    const std::uint8_t raises = bytes[p + schedule + 1];
    schedule += 2 + static_cast<std::size_t>(bets) * 16 + static_cast<std::size_t>(raises) * 16;
  }
  const std::size_t directory_pos = sizes_pos + schedule;
  const std::size_t index_pos = directory_pos + 192;
  // Point the first class offset past the file end.
  const std::uint64_t bogus = static_cast<std::uint64_t>(bytes.size()) + 4096;
  for (int b = 0; b < 8; ++b)
    bytes[index_pos + b] = static_cast<std::uint8_t>((bogus >> (b * 8)) & 0xffu);
  write_bytes(file.path, bytes);
  CHECK(expect_open_refused(file.path));

  // A non-page-aligned offset is likewise refused (bad section alignment).
  auto bytes2 = file.bytes;
  const std::uint64_t unaligned = 4097;
  for (int b = 0; b < 8; ++b)
    bytes2[index_pos + b] = static_cast<std::uint8_t>((unaligned >> (b * 8)) & 0xffu);
  write_bytes(file.path, bytes2);
  CHECK(expect_open_refused(file.path));
  return true;
}

bool test_section_root_mismatch_refused(const fs::path& dir) {
  // A two-root file whose first section names root 1 while its index path is
  // (root 0, class 0) must fail the cross-check. Build directly from staged
  // sections.
  Fixture fx = make_fixture();
  auto range = make_range();
  solver::UnifiedGame game = make_game();
  auto inputs = make_inputs(fx, range);
  for (auto& input : inputs) {
    input.action_abstraction_digest = game.action_id.digest;
    input.card_abstraction_digest = 0x123456789abcdef0ULL;
  }

  std::vector<RootDirectoryEntry> roots{make_root(0),
                                        make_root(1, RootLineTag::SingleRaisedPot, 194, 194, 6, 6)};

  const fs::path staging = dir / "mismatch-staging";
  fs::create_directories(staging);
  std::vector<std::vector<std::uint8_t>> blobs;
  for (std::uint32_t r = 0; r < 2; ++r)
    for (std::uint32_t c = 0; c < 2; ++c) {
      resident::PackClassInput input = inputs[c];
      input.root_id = r;
      input.class_id = c;
      blobs.push_back(
          resident::serialize_class_section(input, resident::ProbabilityLayout::Codebook16));
    }
  // Stage with correct names.
  std::uint32_t idx = 0;
  for (std::uint32_t r = 0; r < 2; ++r)
    for (std::uint32_t c = 0; c < 2; ++c) {
      std::ofstream out(staging / resident::root_class_section_name(r, c), std::ios::binary);
      out.write(reinterpret_cast<const char*>(blobs[idx].data()),
                static_cast<std::streamsize>(blobs[idx].size()));
      ++idx;
    }
  std::vector<std::vector<std::uint32_t>> ids{{0, 1}, {0, 1}};
  const fs::path good_path = dir / "mismatch_good.bin";
  resident::LibraryIdentity identity;
  identity.action_abstraction_digest = game.action_id.digest;
  identity.card_abstraction_digest = 0x123456789abcdef0ULL;
  resident::assemble_binary_library(good_path, game, identity,
                                    resident::ProbabilityLayout::Codebook16, roots, staging, ids);
  CHECK(opens_ok(good_path));

  // Corrupt the first section's root_id word (offset 0).
  auto bytes = read_bytes(good_path);
  bytes[4096] = 1;  // root 0 -> root 1 at class header offset 0
  write_bytes(good_path, bytes);
  CHECK(expect_open_refused(good_path));

  // Nonzero class-header padding (offset 40..47) is a bad effective header.
  bytes = read_bytes(dir / "mismatch_good.bin");
  write_bytes(dir / "pad_bad.bin", bytes);
  auto pad_bytes = read_bytes(dir / "pad_bad.bin");
  pad_bytes[4096 + 44] = 0xff;
  write_bytes(dir / "pad_bad.bin", pad_bytes);
  CHECK(expect_open_refused(dir / "pad_bad.bin"));
  return true;
}

// RFC 0035 cap-gap closure: a cap-1 identity reconstructs the cap in
// game.action_id at open time, and a tokenless recognized line fails closed.
bool test_action_identity_cap(const fs::path& dir) {
  Fixture fx = make_fixture();
  auto range = make_range();
  solver::UnifiedGame game = make_game(/*max_raises=*/1);
  auto inputs = make_inputs(fx, range);
  for (auto& input : inputs) {
    input.action_abstraction_digest = game.action_id.digest;
    input.card_abstraction_digest = 0x123456789abcdef0ULL;
  }
  std::vector<RootDirectoryEntry> roots{make_root(0)};
  const fs::path cap_path = dir / "cap_one.bin";
  resident::pack_binary_library(cap_path, game, roots, std::span(inputs.data(), 2));
  resident::MmapLibrary lib;
  lib.open(cap_path);
  CHECK(lib.action_identity().name == "rfc0007-pot-fractions");
  CHECK(abstraction::parse_max_raises_token(lib.action_identity().parameters) == 1);
  CHECK(abstraction::is_identity_action_family(lib.action_identity()));

  // Hand-edit the parameters to strip the cap token while leaving a recognized
  // family prefix, and fix the digest field to the recomputed value: the
  // strict family predicate must still fail and the mount must be refused.
  auto bytes = read_bytes(cap_path);
  const auto u32 = [&](std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) |
           (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
  };
  std::uint32_t params_len = u32(56);
  const std::size_t params_pos = (64u + u32(48) + 7) & ~std::size_t{7};
  const std::string params(reinterpret_cast<const char*>(bytes.data() + params_pos), params_len);
  const std::string token = "|max-raises=1";
  CHECK(params.ends_with(token));
  const std::string tokenless = params.substr(0, params.size() - token.size());
  CHECK(!tokenless.empty());
  for (std::size_t i = 0; i < tokenless.size(); ++i)
    bytes[params_pos + i] = static_cast<std::uint8_t>(tokenless[i]);
  // Zero the rest of the (padded) parameter field.
  for (std::size_t i = tokenless.size(); i < params_len; ++i)
    bytes[params_pos + i] = 0;
  const std::uint32_t new_len = static_cast<std::uint32_t>(tokenless.size());
  for (int b = 0; b < 4; ++b)
    bytes[56 + b] = static_cast<std::uint8_t>((new_len >> (b * 8)) & 0xffu);
  // The tokenless string does not pass the strict family predicate; the
  // reader rejects before any digest comparison would help.
  write_bytes(cap_path, bytes);
  CHECK(expect_open_refused(cap_path));
  return true;
}

}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "bs_test_mmap_library_v3";
  fs::remove_all(dir);
  fs::create_directories(dir);

  bool ok = true;
  ok = test_pack_and_mmap(dir) && ok;
  ok = test_two_root_library(dir) && ok;
  ok = test_pack_rejects_same_bucket_conflict(dir) && ok;
  ok = test_sha256_verification(dir) && ok;
  ok = test_range_canonicalization(dir) && ok;
  ok = test_assemble_matches_pack(dir) && ok;
  ok = test_assembly_rejects_layout_mismatch(dir) && ok;
  ok = test_old_magic_refused(dir) && ok;
  ok = test_truncation_refused(dir) && ok;
  ok = test_duplicate_root_ledger_refused(dir) && ok;
  ok = test_index_offset_outside_file_refused(dir) && ok;
  ok = test_section_root_mismatch_refused(dir) && ok;
  ok = test_action_identity_cap(dir) && ok;

  fs::remove_all(dir);
  if (!ok || g_failures > 0) {
    std::fprintf(stderr, "mmap_library: %d failures\n", g_failures);
    return 1;
  }
  std::printf("mmap_library: all tests passed\n");
  return 0;
}
