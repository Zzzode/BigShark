// Native NSP decoding tests: valid roundtrips, corrupt rows, and peak byte caps.
#include <zstd.h>

#include <bs/nseat_policy_codec.hpp>
#include <bs/postflop_config.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>

#define CHECK(condition)                                                  \
  do {                                                                    \
    if (!(condition)) {                                                   \
      std::printf("CHECK failed: %s at line %d\n", #condition, __LINE__); \
      return 1;                                                           \
    }                                                                     \
  } while (0)

namespace {
using namespace bs;
using namespace bs::solver;

struct TestFiles {
  std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("bs_policy_codec_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TestFiles() { std::filesystem::create_directory(dir); }
  ~TestFiles() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

template <typename T>
void put(std::vector<char>& bytes, std::size_t offset, T value) {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
    throw std::runtime_error("test mutation outside fixture");
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

template <typename T>
T get(const std::vector<char>& bytes, std::size_t offset) {
  T value;
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
    throw std::runtime_error("test read outside fixture");
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

std::vector<char> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in || in.tellg() < 0)
    throw std::runtime_error("cannot read fixture");
  std::vector<char> bytes(static_cast<std::size_t>(in.tellg()));
  in.seekg(0);
  in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!in)
    throw std::runtime_error("cannot read fixture bytes");
  return bytes;
}

void write_file(const std::filesystem::path& path, const std::vector<char>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!out)
    throw std::runtime_error("cannot write fixture");
}

std::vector<char> decompress(const std::vector<char>& bytes) {
  std::vector<char> raw(get<std::uint64_t>(bytes, 8));
  const auto result = ZSTD_decompress(raw.data(), raw.size(), bytes.data() + 16, bytes.size() - 16);
  if (ZSTD_isError(result) || result != raw.size())
    throw std::runtime_error("cannot decompress fixture");
  return raw;
}

std::vector<char> compress(const std::vector<char>& raw) {
  std::vector<char> bytes(16 + ZSTD_compressBound(raw.size()));
  std::memcpy(bytes.data(), "NSPZ", 4);
  put<std::uint32_t>(bytes, 4, 1);
  put<std::uint64_t>(bytes, 8, raw.size());
  const auto result =
      ZSTD_compress(bytes.data() + 16, bytes.size() - 16, raw.data(), raw.size(), 3);
  if (ZSTD_isError(result))
    throw std::runtime_error("cannot compress fixture");
  bytes.resize(16 + result);
  return bytes;
}

int check_same(const NSeatPolicy& original, const NSeatPolicy& loaded) {
  CHECK(poker::same_game_def(original.game(), loaded.game()));
  CHECK(original.action_id() == loaded.action_id());
  CHECK(original.rows().size() == loaded.rows().size());
  for (const auto& [key, row] : original.rows()) {
    const auto found = loaded.rows().find(key);
    CHECK(found != loaded.rows().end());
    CHECK(row.actions == found->second.actions);
    CHECK(row.probabilities == found->second.probabilities);
    CHECK(found->second.visits == 0);
  }
  return 0;
}

bool rejects(const std::filesystem::path& path, const poker::GameDef& game,
             const abstraction::AbstractionId& id, std::size_t bytes = std::size_t{1} << 30,
             bool resource_only = false) {
  try {
    (void)load_nseat_policy(path, game, id, 2, bytes);
  } catch (const tree::tree_resource_exhausted&) {
    return true;
  } catch (const std::runtime_error&) {
    return !resource_only;
  }
  return false;
}
}  // namespace

int main() {
  TestFiles files;
  const auto path = files.dir / "policy.nsp";
  const auto game = gto::make_postflop_def({0, 1, 10}, 0);
  const abstraction::AbstractionId id{"test", 1, "parameters", 123};
  std::map<NSeatInformationKey, NSeatPolicyRow> rows;
  rows.emplace(NSeatInformationKey{1, 100},
               NSeatPolicyRow{
                   {{poker::ActionType::Check, 0}, {poker::ActionType::Bet, 50}}, {0.7, 0.3}, 42});
  // Existing NSP files may retain a nonzero call target, which is ignored by L1.
  rows.emplace(NSeatInformationKey{2, 200},
               NSeatPolicyRow{
                   {{poker::ActionType::Fold, 0}, {poker::ActionType::Call, 50}}, {0.4, 0.6}, 99});
  const auto original = NSeatPolicy::from_parts(game, id, std::move(rows));
  save_nseat_policy(original, 2, path);
  const auto compressed = read_file(path);
  const auto raw = decompress(compressed);
  const auto loaded = load_nseat_policy(path, game, id, 2);
  CHECK(check_same(original, loaded) == 0);
  write_file(path, raw);
  CHECK(check_same(original, load_nseat_policy(path, game, id, 2)) == 0);
  // Valid NSP rows need not arrive sorted. An insertion hint must fall back.
  auto reversed = raw;
  CHECK(reversed.size() == 140);
  std::swap_ranges(reversed.begin() + 36, reversed.begin() + 88, reversed.begin() + 88);
  write_file(path, reversed);
  CHECK(check_same(original, load_nseat_policy(path, game, id, 2)) == 0);
  write_file(path, compress(reversed));
  CHECK(check_same(original, load_nseat_policy(path, game, id, 2)) == 0);
  write_file(path, raw);
  const auto retained = nseat_policy_accounted_bytes(loaded);
  const auto exact_peak = raw.size() + retained;
  CHECK(check_same(original, load_nseat_policy(path, game, id, 2, exact_peak)) == 0);
  CHECK(rejects(path, game, id, exact_peak - 1, true));
  write_file(path, compressed);
  CHECK(check_same(original, load_nseat_policy(path, game, id, 2, exact_peak)) == 0);
  CHECK(rejects(path, game, id, exact_peak - 1, true));

  constexpr std::size_t first = 36;
  constexpr std::size_t second = 88;
  constexpr std::size_t probability = 72;
  CHECK(raw.size() == 140);
  using Mutation = std::pair<const char*, std::function<void(std::vector<char>&)>>;
  const std::vector<Mutation> mutations = {
      {"magic", [](auto& b) { b[0] = 'X'; }},
      {"version", [](auto& b) { put<std::uint32_t>(b, 4, 99); }},
      {"seat count", [](auto& b) { b[8] = static_cast<char>(255); }},
      {"bucket", [](auto& b) { b[9] = 3; }},
      {"reserved", [](auto& b) { b[10] = 1; }},
      {"flop", [](auto& b) { put<std::int32_t>(b, 16, 52); }},
      {"row count",
       [](auto& b) { put<std::uint32_t>(b, 12, std::numeric_limits<std::uint32_t>::max()); }},
      {"zero card bucket", [](auto& b) { put<std::uint32_t>(b, first, 0); }},
      {"invalid card bucket", [](auto& b) { put<std::uint32_t>(b, first, 10); }},
      {"node sentinel",
       [](auto& b) {
         put<std::uint64_t>(b, first + 4, std::numeric_limits<std::uint64_t>::max());
       }},
      {"duplicate key",
       [](auto& b) {
         put<std::uint32_t>(b, second, 1);
         put<std::uint64_t>(b, second + 4, 100);
       }},
      {"empty action row", [](auto& b) { put<std::uint32_t>(b, first + 12, 0); }},
      {"huge action row",
       [](auto& b) {
         put<std::uint32_t>(b, first + 12, std::numeric_limits<std::uint32_t>::max());
       }},
      {"action enum", [](auto& b) { b[first + 20] = 99; }},
      {"zero aggressive target", [](auto& b) { put<std::uint32_t>(b, first + 32, 0); }},
      {"duplicate action",
       [](auto& b) {
         b[first + 28] = static_cast<char>(poker::ActionType::Check);
         put<std::uint32_t>(b, first + 32, 0);
       }},
      {"nan",
       [](auto& b) { put<double>(b, probability, std::numeric_limits<double>::quiet_NaN()); }},
      {"infinity",
       [](auto& b) { put<double>(b, probability, std::numeric_limits<double>::infinity()); }},
      {"negative probability", [](auto& b) { put<double>(b, probability, -0.1); }},
      {"probability above one", [](auto& b) { put<double>(b, probability, 1.1); }},
      {"unnormalized", [](auto& b) { put<double>(b, probability, 0.5); }},
      {"truncated row", [](auto& b) { b.resize(60); }},
      {"truncated probability", [](auto& b) { b.pop_back(); }},
      {"trailing bytes", [](auto& b) { b.push_back(0); }},
  };
  for (const auto& [name, mutate] : mutations) {
    auto bytes = raw;
    mutate(bytes);
    write_file(path, bytes);
    if (!rejects(path, game, id)) {
      std::printf("unexpected acceptance: %s\n", name);
      return 1;
    }
  }
  for (std::size_t size = 0; size < 36; ++size) {
    write_file(path, std::vector<char>(raw.begin(), raw.begin() + size));
    CHECK(rejects(path, game, id));
  }
  for (std::size_t size = 0; size <= 16; ++size) {
    write_file(path, std::vector<char>(compressed.begin(), compressed.begin() + size));
    CHECK(rejects(path, game, id));
  }
  auto bytes = compressed;
  put<std::uint64_t>(bytes, 8, std::numeric_limits<std::uint64_t>::max());
  write_file(path, bytes);
  CHECK(rejects(path, game, id, exact_peak, true));
  bytes = compressed;
  put<std::uint64_t>(bytes, 8, raw.size() + 1);
  write_file(path, bytes);
  CHECK(rejects(path, game, id));
  bytes = compressed;
  bytes.pop_back();
  write_file(path, bytes);
  CHECK(rejects(path, game, id));
  bytes = compressed;
  bytes.push_back(0);
  write_file(path, bytes);
  CHECK(rejects(path, game, id));
  // Both buffers count simultaneously, even if each alone fits the cap.
  bytes = raw;
  bytes.resize(4096, 0);
  const auto padded_compressed = compress(bytes);
  write_file(path, padded_compressed);
  CHECK(padded_compressed.size() < bytes.size());
  CHECK(rejects(path, game, id, bytes.size(), true));

  save_nseat_policy(original, 0, path);
  CHECK(check_same(original, load_nseat_policy(path, game, id, 5)) == 0);
  const auto empty = NSeatPolicy::from_parts(game, id, {});
  save_nseat_policy(empty, 2, path);
  CHECK(check_same(empty, load_nseat_policy(path, game, id, 2)) == 0);
  poker::GameDef preflop{};
  preflop.player_count = 2;
  preflop.button = 0;
  preflop.big_blind = 2;
  preflop.stacks = {4, 4};
  preflop.blinds_posted = {1, 2};
  preflop.pot = 3;
  preflop.preflop = true;
  std::map<NSeatInformationKey, NSeatPolicyRow> preflop_rows;
  preflop_rows.emplace(NSeatInformationKey{0, 0}, original.rows().begin()->second);
  preflop_rows.emplace(NSeatInformationKey{168, 1}, original.rows().rbegin()->second);
  const auto preflop_policy = NSeatPolicy::from_parts(preflop, id, std::move(preflop_rows));
  save_nseat_policy(preflop_policy, 2, path);
  CHECK(check_same(preflop_policy, load_nseat_policy(path, preflop, id, 2)) == 0);
  bytes = decompress(read_file(path));
  put<std::uint32_t>(bytes, first, 169);
  write_file(path, bytes);
  CHECK(rejects(path, preflop, id));
  auto spacious_rows = original.rows();
  auto& spacious = spacious_rows.begin()->second;
  spacious.actions.reserve(32);
  spacious.probabilities.reserve(32);
  const auto spacious_policy = NSeatPolicy::from_parts(game, id, std::move(spacious_rows));
  CHECK(nseat_policy_accounted_bytes(spacious_policy) > nseat_policy_accounted_bytes(original));
  std::printf(
      "nseat policy byte limits: PASS (%zu mutations, raw/compressed/header/resource cases)\n",
      mutations.size());
  return 0;
}
