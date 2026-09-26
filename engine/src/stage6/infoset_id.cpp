#include <array>
#include <bs/stage6/infoset_id.hpp>
#include <sstream>

namespace bs::stage6 {

namespace {

std::uint64_t fnv1a(const std::string& bytes) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (unsigned char byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

void eat_u64(std::string* out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    *out += static_cast<char>((value >> shift) & 0xffULL);
  }
}

}  // namespace

std::string InfosetUuid::to_string() const {
  std::ostringstream os;
  os << std::hex << hi << ':' << lo;
  return os.str();
}

InfosetUuid AbstractInfosetKey::uuid() const {
  std::string bytes;
  bytes += "abs1";
  eat_u64(&bytes, artifact_content_hash);
  eat_u64(&bytes, static_cast<std::uint64_t>(tree_node_index));
  eat_u64(&bytes, static_cast<std::uint64_t>(own_card_bucket));
  eat_u64(&bytes, path_hash);
  InfosetUuid id;
  id.hi = fnv1a(bytes);
  id.lo = fnv1a(bytes + "|lo");
  return id;
}

InfosetUuid ExactInfosetKey::uuid() const {
  std::string bytes;
  bytes += "exact1";
  eat_u64(&bytes, public_history_hash);
  eat_u64(&bytes, static_cast<std::uint64_t>(static_cast<uint32_t>(own_holding_sorted[0])));
  eat_u64(&bytes, static_cast<std::uint64_t>(static_cast<uint32_t>(own_holding_sorted[1])));
  eat_u64(&bytes, traverser);
  InfosetUuid id;
  id.hi = fnv1a(bytes);
  id.lo = fnv1a(bytes + "|lo");
  return id;
}

std::uint64_t public_history_hash(const HandLog& log, std::span<const int> board,
                                  poker::Street street) {
  // Delegate to the canonical token builder in infoset_key.cpp so there is one
  // derivation. A rooted GameState carrying `board` plus this log is the exact
  // information set; this helper rebuilds only the hash without a GameState.
  (void)street;
  std::string bytes;
  auto eat = [&bytes](std::uint64_t value) { eat_u64(&bytes, value); };
  eat(static_cast<std::uint64_t>(board.size()));
  for (int card : board)
    eat(static_cast<std::uint64_t>(card) + 1);
  static constexpr std::uint64_t tags[4] = {0x5052ULL, 0x464cULL, 0x5455ULL, 0x5249ULL};
  const std::vector<LoggedAction>* logs[4] = {&log.preflop, &log.flop, &log.turn, &log.river};
  for (std::size_t s = 0; s < 4; ++s) {
    eat(tags[s]);
    eat(logs[s]->size());
    for (const LoggedAction& a : *logs[s]) {
      eat(a.seat + 1);
      eat(static_cast<std::uint64_t>(static_cast<int>(a.action.type)) + 1);
      eat(a.action.target_total + 1);
    }
  }
  return fnv1a(bytes);
}

}  // namespace bs::stage6
