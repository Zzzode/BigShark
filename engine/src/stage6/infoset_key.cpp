#include <algorithm>
#include <bs/stage6/infoset_key.hpp>
#include <sstream>

namespace bs::stage6 {
namespace {

namespace poker = bs::poker;

const std::vector<LoggedAction>& street_log(const HandLog& log, poker::Street street) {
  switch (street) {
    case poker::Street::Preflop:
      return log.preflop;
    case poker::Street::Flop:
      return log.flop;
    case poker::Street::Turn:
      return log.turn;
    case poker::Street::River:
      return log.river;
  }
  return log.river;
}

std::uint64_t street_tag(poker::Street street) {
  // Distinct nonzero tags; +1 offset keeps them off the action-type range.
  switch (street) {
    case poker::Street::Preflop:
      return 0x5052ULL;  // "PR"
    case poker::Street::Flop:
      return 0x464cULL;  // "FL"
    case poker::Street::Turn:
      return 0x5455ULL;  // "TU"
    case poker::Street::River:
      return 0x5249ULL;  // "RI"
  }
  return 0;
}

}  // namespace

std::vector<std::uint64_t> public_history_tokens(const poker::GameState& state,
                                                 const HandLog& log) {
  std::vector<std::uint64_t> tokens;
  const std::span<const int> board = state.board();
  tokens.push_back(static_cast<std::uint64_t>(board.size()));
  for (int card : board)
    tokens.push_back(static_cast<std::uint64_t>(card) + 1);
  const poker::Street streets[4] = {poker::Street::Preflop, poker::Street::Flop,
                                    poker::Street::Turn, poker::Street::River};
  for (poker::Street street : streets) {
    const std::vector<LoggedAction>& actions = street_log(log, street);
    tokens.push_back(street_tag(street));
    tokens.push_back(actions.size());
    for (const LoggedAction& a : actions) {
      tokens.push_back(static_cast<std::uint64_t>(a.seat) + 1);
      tokens.push_back(static_cast<std::uint64_t>(static_cast<int>(a.action.type)) + 1);
      tokens.push_back(static_cast<std::uint64_t>(a.action.target_total) + 1);
    }
  }
  return tokens;
}

bool InfosetKey::operator<(const InfosetKey& other) const noexcept {
  if (own[0] != other.own[0])
    return own[0] < other.own[0];
  if (own[1] != other.own[1])
    return own[1] < other.own[1];
  return std::lexicographical_compare(public_tokens.begin(), public_tokens.end(),
                                      other.public_tokens.begin(), other.public_tokens.end());
}

std::string InfosetKey::canonical() const {
  std::ostringstream os;
  os << own[0] << ',' << own[1] << '|';
  for (std::uint64_t t : public_tokens)
    os << t << ';';
  return os.str();
}

std::uint64_t InfosetKey::content_hash() const noexcept {
  const std::string bytes = canonical();
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (unsigned char byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

InfosetKey make_infoset_key(const poker::GameState& state, const HandLog& log,
                            std::size_t /*traverser*/, HoleCards own) {
  InfosetKey key;
  key.own = {std::min(own[0], own[1]), std::max(own[0], own[1])};
  key.public_tokens = public_history_tokens(state, log);
  return key;
}

std::uint64_t public_history_hash(const poker::GameState& state, const HandLog& log) {
  // Length-framed FNV-1a over the token stream; the per-token length prefix
  // keeps distinct streams from framing to the same bytes.
  const std::vector<std::uint64_t> tokens = public_history_tokens(state, log);
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  auto eat = [&](std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      hash ^= static_cast<unsigned char>((value >> shift) & 0xffULL);
      hash *= 0x100000001b3ULL;
    }
  };
  eat(tokens.size());
  for (std::uint64_t token : tokens)
    eat(token);
  return hash;
}

}  // namespace bs::stage6
