#include <array>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/strategy_artifact.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "artifact_internal.hpp"

namespace bs::artifacts {

using poker::ActionType;
using solver::InformationKey;

namespace {

void fail(ArtifactErrorKind kind, const std::string& message) {
  throw ArtifactError(kind, message);
}

void require_key(bool ok, const std::string& message) {
  if (!ok)
    fail(ArtifactErrorKind::InvalidSchema, message);
}

// Splits on a single-character delimiter, preserving empty fields.
std::vector<std::string_view> split(std::string_view text, char delimiter,
                                    std::size_t expected_parts) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  for (;;) {
    const std::size_t at = text.find(delimiter, start);
    if (at == std::string_view::npos) {
      parts.push_back(text.substr(start));
      break;
    }
    parts.push_back(text.substr(start, at - start));
    start = at + 1;
  }
  require_key(parts.size() == expected_parts,
              "canonical key must have street|boards|events fields");
  return parts;
}

std::uint64_t parse_decimal(std::string_view field, const std::string& what) {
  require_key(!field.empty(), "empty numeric field in canonical key: " + what);
  require_key(field.size() == 1 || field[0] != '0', "leading zero in canonical key: " + what);
  std::uint64_t value = 0;
  for (char ch : field) {
    require_key(ch >= '0' && ch <= '9', "non-decimal digit in canonical key: " + what);
    const unsigned digit = static_cast<unsigned>(ch - '0');
    require_key(value <= (kMaxStoredChips - digit) / 10,
                "amount exceeds 2^53-1 in canonical key: " + what);
    value = value * 10 + digit;
  }
  return value;
}

// The two frozen key grammars. Revision 1 is the heads-up grammar this codec
// has always emitted; revision 2 widens the seat and street domains for the
// seat-generic artifact. The street<->board_count mapping is the one place the
// two genuinely differ beyond a bound: revision 2 admits board_count 0 as the
// preflop form, which encodes as street token 3.
struct KeyGrammar {
  std::uint32_t revision = 0;
  int max_seat = 0;            // highest legal actor/player index
  int max_street = 0;          // highest legal street token
  bool admit_preflop = false;  // board_count 0 <-> street 3
};

KeyGrammar grammar_for_revision(std::uint32_t revision) {
  if (revision == 1)
    return {1, 1, 2, false};
  if (revision == 2)
    return {2, 9, 3, true};
  fail(ArtifactErrorKind::InvalidArgument,
       "unsupported information key grammar revision " + std::to_string(revision));
}

// The round-end grammar advances the event street on a call or a check-check.
// Revision 2 wraps preflop (3) back to flop (0); revision 1 (flop-rooted) and
// every non-preflop revision-2 street simply increment, so the two-seat route
// is byte-identical to the historical `++current_street`.
int advance_event_street(const KeyGrammar& grammar, int street) {
  if (grammar.admit_preflop && street == 3)
    return 0;
  return street + 1;
}

}  // namespace

std::string encode_public_key(const InformationKey& key, std::uint32_t revision) {
  const KeyGrammar grammar = grammar_for_revision(revision);
  if (key.size() < 4)
    fail(ArtifactErrorKind::InvalidArgument, "information key too short");
  const std::uint64_t board_count = key[3];
  const bool board_count_ok = grammar.admit_preflop
                                  ? (board_count == 0 || (board_count >= 3 && board_count <= 5))
                                  : (board_count >= 3 && board_count <= 5);
  if (!board_count_ok)
    fail(ArtifactErrorKind::InvalidArgument, "information key board size out of range");
  if (key.size() < 4 + board_count ||
      (key.size() - 4 - static_cast<std::size_t>(board_count)) % 4 != 0)
    fail(ArtifactErrorKind::InvalidArgument, "malformed information key event vector");

  // Revision 2 encodes the preflop form (board_count 0) as street token 3;
  // every other board_count maps to street = board_count - 3 in both grammars.
  const int street =
      (grammar.admit_preflop && board_count == 0) ? 3 : static_cast<int>(board_count) - 3;
  std::string text;
  text += std::to_string(street);
  text.push_back('|');
  for (std::size_t i = 0; i < board_count; ++i) {
    if (i != 0)
      text.push_back(',');
    text += std::to_string(key[4 + i]);
  }
  text.push_back('|');

  const std::size_t events_begin = 4 + static_cast<std::size_t>(board_count);
  for (std::size_t i = events_begin; i < key.size(); i += 4) {
    if (i != events_begin)
      text.push_back(',');
    const std::uint64_t actor = key[i + 1];
    const std::uint64_t type = key[i + 2];
    const std::uint64_t target = key[i + 3];
    if (actor > static_cast<std::uint64_t>(grammar.max_seat) || type > 4)
      fail(ArtifactErrorKind::InvalidArgument, "information key event out of range");
    text += std::to_string(actor);
    text.push_back(':');
    text.push_back(detail::action_kind_char(static_cast<ActionType>(type)));
    text.push_back(':');
    if (type == static_cast<std::uint64_t>(ActionType::Bet) ||
        type == static_cast<std::uint64_t>(ActionType::Raise))
      text += std::to_string(target);
    else
      text.push_back('-');
    if (grammar.revision == 2) {
      // Revision 2 carries the event's street as a fourth field. Revision 1
      // reconstructs it with the round-end rule (advance on a call or a
      // check-check), which is a two-seat rule: with three or more seats a
      // check-check or a non-final call does not close the round, so the
      // street cannot be recovered from actor/kind/target alone.
      const std::uint64_t event_street = key[i];
      if (event_street > static_cast<std::uint64_t>(grammar.max_street))
        fail(ArtifactErrorKind::InvalidArgument, "information key event street out of range");
      text.push_back(':');
      text += std::to_string(event_street);
    }
  }
  return text;
}

InformationKey decode_public_key(const std::string& text, std::uint32_t revision, int player,
                                 int card0, int card1) {
  const KeyGrammar grammar = grammar_for_revision(revision);
  if (player < 0 || player > grammar.max_seat)
    fail(ArtifactErrorKind::InvalidSchema, "information state player outside seats");
  if (card0 < 0 || card0 >= card1 || card1 > 51)
    fail(ArtifactErrorKind::InvalidSchema, "information state own cards out of range");
  for (char ch : text) {
    const bool ok = (ch >= '0' && ch <= '9') || ch == '|' || ch == ',' || ch == ':' || ch == '-' ||
                    ch == 'f' || ch == 'x' || ch == 'c' || ch == 'b' || ch == 'r';
    require_key(ok, "unexpected character in canonical key");
  }
  const auto fields = split(text, '|', 3);

  const std::uint64_t street = parse_decimal(fields[0], "street");
  require_key(street <= static_cast<std::uint64_t>(grammar.max_street),
              "canonical key street out of range");
  // Revision 2 admits the preflop form (street token 3, board_count 0); every
  // other street maps to board_count = 3 + street in both grammars.
  const bool is_preflop = grammar.admit_preflop && street == 3;
  const std::size_t board_count = is_preflop ? 0 : 3 + static_cast<std::size_t>(street);

  std::vector<std::uint64_t> boards;
  boards.reserve(board_count);
  if (board_count == 0) {
    require_key(fields[1].empty(), "preflop key must have an empty board field");
  } else {
    std::size_t start = 0;
    for (;;) {
      const auto at = fields[1].find(',', start);
      const auto token = at == std::string_view::npos ? fields[1].substr(start)
                                                      : fields[1].substr(start, at - start);
      const std::uint64_t card = parse_decimal(token, "board card");
      require_key(card < 52, "board card out of range");
      boards.push_back(card);
      if (at == std::string_view::npos)
        break;
      start = at + 1;
    }
  }
  require_key(boards.size() == board_count, "board count does not match key street");
  for (std::size_t a = 0; a < boards.size(); ++a) {
    require_key(static_cast<int>(boards[a]) != card0 && static_cast<int>(boards[a]) != card1,
                "own combo card appears on public board");
    for (std::size_t b = a + 1; b < boards.size(); ++b)
      require_key(boards[a] != boards[b], "duplicate card on the public board");
  }

  InformationKey key{static_cast<std::uint64_t>(player), static_cast<std::uint64_t>(card0),
                     static_cast<std::uint64_t>(card1), static_cast<std::uint64_t>(board_count)};
  key.insert(key.end(), boards.begin(), boards.end());

  // Revision 2's preflop form starts event streets at 3 (preflop); every other
  // key (and all revision-1 keys) is flop-rooted and starts at 0.
  int current_street = is_preflop ? 3 : 0;
  bool previous_was_check = false;
  bool terminal = false;
  // Revision 2 carries each event's street explicitly; it must be non-decreasing
  // (the preflop->flop wrap 3->0 is the one allowed decrease).
  std::uint64_t prev_event_street = static_cast<std::uint64_t>(current_street);
  if (!fields[2].empty()) {
    std::size_t start = 0;
    for (;;) {
      const auto comma = fields[2].find(',', start);
      const auto triple = comma == std::string_view::npos ? fields[2].substr(start)
                                                          : fields[2].substr(start, comma - start);
      // Revision 2 events carry a fourth street field; revision 1 events do not.
      const auto parts = split(triple, ':', grammar.revision == 2 ? 4 : 3);
      const std::uint64_t actor = parse_decimal(parts[0], "event actor");
      require_key(actor <= static_cast<std::uint64_t>(grammar.max_seat),
                  "event actor outside seats");
      require_key(parts[1].size() == 1, "event kind must be one character");
      const char kind = parts[1][0];
      require_key(!terminal, "events present after a fold");
      ActionType type;
      switch (kind) {
        case 'f':
          type = ActionType::Fold;
          break;
        case 'x':
          type = ActionType::Check;
          break;
        case 'c':
          type = ActionType::Call;
          break;
        case 'b':
          type = ActionType::Bet;
          break;
        case 'r':
          type = ActionType::Raise;
          break;
        case 'd':
          fail(ArtifactErrorKind::InvalidSchema,
               "revision-1 keys encode public deals in board_ids, not d events");
        default:
          fail(ArtifactErrorKind::InvalidSchema, "unknown canonical event kind");
      }
      std::uint64_t target = 0;
      if (parts[2] == "-") {
        require_key(type != ActionType::Bet && type != ActionType::Raise,
                    "aggressive event requires a target total");
      } else {
        require_key(type == ActionType::Bet || type == ActionType::Raise,
                    "non-aggressive event must have target -");
        target = parse_decimal(parts[2], "event target");
        require_key(target > 0, "aggressive target must be positive");
      }

      std::uint64_t event_street;
      if (grammar.revision == 2) {
        // The street is explicit: the round-end reconstruction revision 1 uses
        // is a two-seat rule and cannot recover it for three or more seats.
        event_street = parse_decimal(parts[3], "event street");
        require_key(event_street <= static_cast<std::uint64_t>(grammar.max_street),
                    "event street out of range");
        require_key(event_street <= street, "event street exceeds key street");
        const bool wraps_preflop = prev_event_street == 3 && event_street == 0;
        require_key(event_street >= prev_event_street || wraps_preflop,
                    "event streets out of order");
        prev_event_street = event_street;
      } else {
        event_street = static_cast<std::uint64_t>(current_street);
      }
      key.push_back(event_street);
      key.push_back(actor);
      key.push_back(static_cast<std::uint64_t>(type));
      key.push_back(target);

      if (grammar.revision == 2) {
        // A fold ends the hand only in heads-up; with three or more seats the
        // remaining players continue, so revision 2 does not treat a fold as
        // terminal and applies no round-end reconstruction.
      } else if (type == ActionType::Fold) {
        terminal = true;
      } else if (type == ActionType::Check) {
        if (previous_was_check) {
          current_street = advance_event_street(grammar, current_street);
          previous_was_check = false;
        } else {
          previous_was_check = true;
        }
      } else if (type == ActionType::Call) {
        current_street = advance_event_street(grammar, current_street);
        previous_was_check = false;
      } else {
        previous_was_check = false;
      }
      if (grammar.revision != 2)
        require_key(current_street <= static_cast<int>(street), "event street exceeds key street");
      if (comma == std::string_view::npos)
        break;
      start = comma + 1;
    }
  }
  if (grammar.revision != 2)
    require_key(current_street == static_cast<int>(street),
                "event round closures do not match key street");
  return key;
}

namespace detail {

std::string u64_to_hex(std::uint64_t value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text(16, '0');
  for (int i = 15; i >= 0; --i) {
    text[i] = kDigits[value & 0xF];
    value >>= 4;
  }
  return text;
}

std::uint64_t hex_to_u64(std::string_view text) {
  if (text.size() != 16)
    throw ArtifactError(ArtifactErrorKind::InvalidSchema,
                        "PRNG word must be exactly 16 lowercase hex digits");
  std::uint64_t value = 0;
  for (char ch : text) {
    unsigned digit;
    if (ch >= '0' && ch <= '9')
      digit = static_cast<unsigned>(ch - '0');
    else if (ch >= 'a' && ch <= 'f')
      digit = static_cast<unsigned>(ch - 'a' + 10);
    else
      throw ArtifactError(ArtifactErrorKind::InvalidSchema,
                          "PRNG word must use lowercase hexadecimal");
    value = (value << 4) | digit;
  }
  return value;
}

bool is_hex_word(std::string_view text) {
  if (text.size() != 16)
    return false;
  for (char ch : text)
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
      return false;
  return true;
}

int action_kind_id(ActionType type) {
  return static_cast<int>(type);  // enum order is the pinned schema order.
}

ActionType action_kind_from_id(int id) {
  if (id < 0 || id > 4)
    throw ArtifactError(ArtifactErrorKind::InvalidSchema, "action kind out of range");
  return static_cast<ActionType>(id);
}

char action_kind_char(ActionType type) {
  switch (type) {
    case ActionType::Fold:
      return 'f';
    case ActionType::Check:
      return 'x';
    case ActionType::Call:
      return 'c';
    case ActionType::Bet:
      return 'b';
    case ActionType::Raise:
      return 'r';
  }
  return '?';
}

int combo_id(std::array<int, 2> cards) {
  if (cards[0] < 0 || cards[1] > 51 || cards[0] >= cards[1])
    throw ArtifactError(ArtifactErrorKind::InvalidArgument, "invalid combo cards");
  // Zero-based index of the unordered pair (i, j), i < j, among the
  // 52*51/2 = 1326 combos: sum over smaller first cards plus position within
  // the first-card-i run (j - i - 1).
  const int i = cards[0];
  return i * 50 - i * (i - 1) / 2 + cards[1] - 1;
}

std::array<int, 2> combo_cards(int combo) {
  if (combo < 0 || combo > 1325)
    throw ArtifactError(ArtifactErrorKind::InvalidSchema, "combo id out of range");
  for (int i = 0; i <= 50; ++i) {
    const int base = i * 50 - i * (i - 1) / 2;
    // Within first-card-i, j runs i+1..51, so ids are base+i .. base+50.
    if (combo <= base + 50)
      return {i, combo - base + 1};
  }
  throw ArtifactError(ArtifactErrorKind::InvalidSchema, "combo id has no pair");
}

double require_finite(double value, const std::string& field) {
  if (!std::isfinite(value))
    throw ArtifactError(ArtifactErrorKind::InvalidValue, "non-finite REAL in " + field);
  return value;
}

void validate_probabilities(const std::vector<double>& probabilities, const std::string& field) {
  if (probabilities.empty() || probabilities.size() > 32)
    throw ArtifactError(ArtifactErrorKind::InvalidValue,
                        field + ": action count must be between 1 and 32");
  double sum = 0;
  for (double probability : probabilities) {
    if (!std::isfinite(probability) || probability < 0 || probability > 1)
      throw ArtifactError(ArtifactErrorKind::InvalidValue, field + ": probability outside [0,1]");
    sum += probability;
  }
  if (std::abs(sum - 1.0) > kProbabilitySumTolerance)
    throw ArtifactError(ArtifactErrorKind::InvalidValue,
                        field + ": probabilities do not sum to 1 within 1e-12");
}

void PolicyAssembler::set_game(solver::HeadsUpPolicy& policy, const solver::HeadsUpGame& game) {
  policy.game_ = game;
}

void PolicyAssembler::add_row(solver::HeadsUpPolicy& policy, InformationKey key,
                              solver::PolicyRow row) {
  policy.rows_.emplace(std::move(key), std::move(row));
}

void SeatPolicyAssembler::set_identity(solver::SeatPolicy& policy, poker::GameDef game,
                                       abstraction::SizeSchedule sizes,
                                       std::vector<std::vector<solver::WeightedHand>> ranges,
                                       abstraction::AbstractionId action_id) {
  policy.game_ = std::move(game);
  policy.sizes_ = std::move(sizes);
  policy.ranges_ = std::move(ranges);
  policy.action_id_ = std::move(action_id);
}

void SeatPolicyAssembler::add_row(solver::SeatPolicy& policy, solver::InformationKey key,
                                  solver::SeatPolicyRow row) {
  policy.rows_.emplace(std::move(key), std::move(row));
}

}  // namespace detail

}  // namespace bs::artifacts
