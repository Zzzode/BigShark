#include <algorithm>
#include <array>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <cstdint>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace bs::abstraction {
namespace {

using poker::Action;
using poker::ActionType;
using poker::Chips;

// Overflow-checked chip sum, identical in contract to the rules layer's
// detail::add; duplicated here because L2 must not reach into poker's private
// detail header.
Chips checked_add(Chips a, Chips b) {
  if (a > std::numeric_limits<Chips>::max() - b)
    throw std::overflow_error("chip sum overflow");
  return a + b;
}

// Ceiling of amount * (n/d), requiring a positive reduced fraction -- the exact
// shipped RFC 0007 rounding so an identity menu is bit-for-bit the old one.
Chips ceil_fraction(Chips amount, const Fraction& f) {
  if (f.numerator == 0 || f.denominator == 0 || std::gcd(f.numerator, f.denominator) != 1)
    throw std::invalid_argument("sizes must be positive reduced fractions");
  if (amount > std::numeric_limits<Chips>::max() / f.numerator)
    throw std::overflow_error("size product overflow");
  const Chips product = amount * f.numerator;
  return product / f.denominator + (product % f.denominator != 0);
}

// Deterministic canonical text of a schedule's parameters. Fractions are
// reduced first so mathematically identical schedules (2/4 and 1/2) mint the
// SAME id -- reduced form is part of declared identity, enforced here rather
// than only later when a menu is built. A non-positive fraction is rejected at
// declaration too: this runs in ActionAbstraction's constructor, ahead of the
// menu-build validation in ceil_fraction, so reducing gcd(0,0)==0 must never
// divide by zero.
std::string canonical_fraction(Fraction f) {
  if (f.numerator == 0 || f.denominator == 0)
    throw std::invalid_argument("sizes must be positive reduced fractions");
  const auto g = std::gcd(f.numerator, f.denominator);
  f.numerator /= g;
  f.denominator /= g;
  return std::to_string(f.numerator) + '/' + std::to_string(f.denominator);
}

std::string canonical_schedule(const SizeSchedule& schedule) {
  std::ostringstream out;
  out << "size-v1\n";
  for (std::size_t street = 0; street < schedule.size(); ++street) {
    out << "street=" << street << " bets";
    for (const Fraction& f : schedule[street].bets)
      out << ' ' << canonical_fraction(f);
    out << " raises";
    for (const Fraction& f : schedule[street].raises)
      out << ' ' << canonical_fraction(f);
    out << '\n';
  }
  return out.str();
}

std::uint64_t fnv1a(const std::string& bytes) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (unsigned char byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

}  // namespace

SizeSchedule default_size_schedule() {
  SizeSchedule schedule{};
  // Flop/Turn/River keep their StreetSizes default (1/3,3/4,3/2 bets and
  // 1/2,1/1 raises); preflop gets the RFC 0007 blind-relative menu.
  StreetSizes preflop;
  preflop.bets = {{3, 2}, {2, 1}, {3, 1}};
  preflop.raises = {{3, 2}, {2, 1}, {3, 1}};
  schedule[static_cast<std::size_t>(poker::Street::Preflop)] = preflop;
  return schedule;
}

std::uint64_t abstraction_digest(const std::string& name, std::uint32_t version,
                                 const std::string& parameters) {
  std::ostringstream out;
  out << name << '\0' << version << '\0' << parameters;
  return fnv1a(out.str());
}

std::string AbstractionId::to_string() const {
  std::ostringstream out;
  out << name << ":v" << version << ':' << std::hex << digest;
  return out.str();
}

void require_same_abstraction(const AbstractionId& requested, const AbstractionId& trained) {
  if (requested != trained)
    throw abstraction_mismatch(requested.to_string(), trained.to_string());
}

AbstractionId ActionAbstraction::make_id(const SizeSchedule& schedule) {
  AbstractionId id;
  id.name = "rfc0007-pot-fractions";
  id.version = 1;
  id.parameters = canonical_schedule(schedule);
  id.digest = abstraction_digest(id.name, id.version, id.parameters);
  return id;
}

AbstractionId ActionAbstraction::make_declared_id(const SizeSchedule& schedule, CoverSeeds seeds) {
  AbstractionId id;
  id.name = "rfc0008-declared-coarse";
  id.version = 1;
  // The cover-seed rule is part of the declared map: two coarse schedules that
  // differ only in whether the forced min/cap are offered must not share an id.
  std::string params = canonical_schedule(schedule);
  params += seeds == CoverSeeds::DeclaredOnly ? "cover-seeds=declared-only\n"
                                              : "cover-seeds=min-and-cap\n";
  id.parameters = std::move(params);
  id.digest = abstraction_digest(id.name, id.version, id.parameters);
  return id;
}

ActionAbstraction ActionAbstraction::declared(SizeSchedule schedule, CoverSeeds seeds) {
  SizeSchedule reduced_schedule = reduced(std::move(schedule));
  AbstractionId id = make_declared_id(reduced_schedule, seeds);
  return ActionAbstraction(std::move(reduced_schedule), seeds, std::move(id));
}

SizeSchedule ActionAbstraction::reduced(SizeSchedule schedule) {
  auto reduce = [](std::vector<Fraction>& fracs) {
    for (Fraction& frac : fracs) {
      if (frac.numerator == 0 || frac.denominator == 0)
        throw std::invalid_argument("sizes must be positive reduced fractions");
      const auto g = std::gcd(frac.numerator, frac.denominator);
      frac.numerator /= g;
      frac.denominator /= g;
    }
  };
  for (StreetSizes& street : schedule) {
    reduce(street.bets);
    reduce(street.raises);
  }
  return schedule;
}

AbstractionId identity_action_id() {
  return ActionAbstraction::identity().id();
}

AbstractionId card_abstraction_id(CardBucketKind kind) {
  AbstractionId id;
  switch (kind) {
    case CardBucketKind::Identity:
      id.name = "evaluator-score";
      id.version = 1;
      id.parameters = "bucket=current-made-hand-score;lossless=river-terminal";
      break;
    case CardBucketKind::CategoryTiersV1:
      id.name = "category-tiers";
      id.version = 1;
      id.parameters = "bucket=hand-category;tiers=9;lossless=0";
      break;
  }
  id.digest = abstraction_digest(id.name, id.version, id.parameters);
  return id;
}

// Shared ordered-menu core. `cover_total` is the deepest street total a wager
// can be matched to (the one opponent for heads-up; the max over all other live
// non-folded seats for the 3+ profile). Everything after the cap is the exact
// shipped RFC 0007 rule.
static std::vector<Action> build_menu_with_cover(const poker::LegalActions& legal,
                                                 const StreetSizes& street_sizes,
                                                 poker::Street street, poker::Chips pot,
                                                 poker::Chips actor_committed,
                                                 poker::Chips cover_total, CoverSeeds seeds) {
  std::vector<Action> result;
  if (legal.fold)
    result.push_back({ActionType::Fold});
  if (legal.check)
    result.push_back({ActionType::Check});
  if (legal.call)
    result.push_back({ActionType::Call});
  if (!legal.aggressive)
    return result;

  const poker::TargetRange& bounds = *legal.aggressive;
  // If the deepest cover cannot reach a minimum full raise, the rules still
  // require that minimum (unless the actor itself is short); excess is refunded
  // later via side pots.
  const Chips cap = std::max(bounds.minimum, std::min(bounds.maximum, cover_total));
  const Chips base = checked_add(actor_committed, legal.call_amount);
  const Chips pot_after_call = checked_add(pot, legal.call_amount);

  // The identity rule (MinAndCap) always offers the legal minimum and the
  // effective all-in cap. DeclaredOnly starts empty: those targets survive
  // only when a declared fraction clamps onto one. With no fractions the
  // coarse menu is purely passive.
  std::vector<Chips> targets;
  targets.reserve(street_sizes.bets.size() + street_sizes.raises.size() + 2);
  if (seeds == CoverSeeds::MinAndCap)
    targets = {bounds.minimum, cap};
  const std::vector<Fraction>& fractions =
      bounds.type == ActionType::Bet ? street_sizes.bets : street_sizes.raises;
  for (const Fraction& f : fractions) {
    const Chips target = checked_add(base, ceil_fraction(pot_after_call, f));
    targets.push_back(std::clamp(target, bounds.minimum, cap));
  }
  if (targets.empty())
    return result;  // declared-only with no fractions: passive menu only
  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  for (Chips target : targets)
    result.push_back({bounds.type, target});
  if (result.size() > 32)
    // Same exception family the shipped solver guard used (its require() threw
    // invalid_argument), so a direct L2 caller catching oversized menus keeps
    // the pre-move contract. Unreachable through a validated trainer game.
    throw std::invalid_argument("abstract action count exceeds 32");
  return result;
}

std::vector<Action> build_action_menu(const poker::LegalActions& legal,
                                      const StreetSizes& street_sizes, const MenuContext& context,
                                      CoverSeeds seeds) {
  const Chips cover_total = checked_add(context.opponent_committed, context.opponent_stack);
  return build_menu_with_cover(legal, street_sizes, context.street, context.pot,
                               context.actor_committed, cover_total, seeds);
}

std::vector<Action> build_multiway_action_menu(const poker::LegalActions& legal,
                                               const StreetSizes& street_sizes,
                                               const MultiwayMenuContext& context,
                                               CoverSeeds seeds) {
  return build_menu_with_cover(legal, street_sizes, context.street, context.pot,
                               context.actor_committed, context.cover, seeds);
}

std::uint32_t strength_bucket(const std::array<int, 2>& hole, const std::vector<int>& board) {
  // Card bucketing is defined on a flop/turn/river board (3/4/5 public cards);
  // a preflop holding has no board to bucket against and an over-long board
  // would write past the fixed array. Fail closed rather than read OOB.
  if (board.size() < 3 || board.size() > 5)
    throw std::invalid_argument("card bucket requires a 3/4/5-card board");
  // The two hole cards sit immediately after the public cards so the evaluator
  // reads exactly board.size()+2 contiguous cards, never zero padding that a
  // fixed index 5/6 would read on the flop.
  std::array<int, 7> cards{};
  for (std::size_t i = 0; i < board.size(); ++i)
    cards[i] = board[i];
  cards[board.size()] = hole[0];
  cards[board.size() + 1] = hole[1];
  return bs::evaluate(cards.data(), static_cast<int>(board.size()) + 2).score;
}

std::uint32_t card_bucket(CardBucketKind kind, const std::array<int, 2>& hole,
                          const std::vector<int>& board) {
  const std::uint32_t score = strength_bucket(hole, board);
  switch (kind) {
    case CardBucketKind::Identity:
      return score;
    case CardBucketKind::CategoryTiersV1:
      // The evaluator packs the category (1..9) in the top bits; same category
      // collapses to one bucket, discarding every rank/ kicker distinction.
      return (score >> 20) & 0xF;
  }
  return score;
}

}  // namespace bs::abstraction
