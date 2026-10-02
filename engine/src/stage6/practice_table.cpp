#include <array>
#include <bs/behavior_policy.hpp>
#include <bs/prng.hpp>
#include <bs/stage6/baseline_policy.hpp>
#include <bs/stage6/practice_table.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bs::stage6 {
namespace {

namespace poker = bs::poker;

std::vector<LoggedAction>& street_log(HandLog& log, poker::Street street) {
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

// Deterministic per-decision seed (same convention as the HandSimulator so bot
// behavior is reproducible for fixed inputs).
std::uint64_t decision_seed(std::uint64_t hand, std::uint64_t decision) {
  std::uint64_t z = 0x5336312d6465636eULL ^ hand ^ (decision + 1) * 0x9e3779b97f4a7c15ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z ^= z >> 31;
  return z ? z : 1ULL;
}

double unit_draw(bs::SplitMix64& rng) {
  return static_cast<double>(rng.next_u64() >> 11) * 0x1.0p-53;
}

// Uniform rejection-free bounded index (SplitMix64, same convention as the
// rest of the engine).
std::size_t bounded_index(bs::SplitMix64& rng, std::size_t bound) {
  if (bound == 0)
    throw std::runtime_error("practice table sampled from an empty deck");
  const auto limit = static_cast<std::uint64_t>(bound);
  const std::uint64_t threshold = (std::numeric_limits<std::uint64_t>::max() - limit + 1) % limit;
  for (;;) {
    const std::uint64_t value = rng.next_u64();
    if (value >= threshold)
      return static_cast<std::size_t>(value % limit);
  }
}

// Builds the preflop GameDef for one hand: equal starting stacks, SB/BB posted
// by button-relative seats (heads-up posts the SB on the button, like the
// unified rules), board empty.
poker::GameDef make_def(const PracticeConfig& cfg, std::size_t button) {
  const std::size_t n = cfg.seats;
  poker::GameDef def{};
  def.player_count = n;
  def.button = button;
  def.big_blind = cfg.big_blind;
  def.preflop = true;
  for (std::size_t i = 0; i < n; ++i)
    def.stacks[i] = cfg.starting_stack;
  std::array<poker::Chips, 10> blinds{};
  const poker::Chips sb = cfg.big_blind / 2;
  if (n == 2) {
    blinds[button] = sb;
    blinds[(button + 1) % n] = cfg.big_blind;
  } else {
    blinds[(button + 1) % n] = sb;
    blinds[(button + 2) % n] = cfg.big_blind;
  }
  def.blinds_posted = blinds;
  def.pot = std::accumulate(blinds.begin(), blinds.begin() + static_cast<std::ptrdiff_t>(n),
                            poker::Chips{0});
  def.board = {-1, -1, -1, -1, -1};
  def.board_size = 0;
  return def;
}

// Fisher-Yates shuffled 2N hole cards + 5 board cards, all distinct.
struct Deal {
  std::vector<HoleCards> holes;
  std::array<int, 5> board{};
};

Deal deal_cards(std::size_t n, bs::SplitMix64& rng) {
  std::vector<int> deck;
  deck.reserve(52);
  for (int c = 0; c < 52; ++c)
    deck.push_back(c);
  // Partial Fisher-Yates pop: draw 2N+5 cards.
  const std::size_t need = 2 * n + 5;
  for (std::size_t i = 0; i < need; ++i) {
    const std::size_t j = i + bounded_index(rng, deck.size() - i);
    std::swap(deck[i], deck[j]);
  }
  Deal d;
  d.holes.assign(n, HoleCards{0, 0});
  for (std::size_t s = 0; s < n; ++s) {
    d.holes[s][0] = deck[2 * s];
    d.holes[s][1] = deck[2 * s + 1];
  }
  for (std::size_t i = 0; i < 5; ++i)
    d.board[i] = deck[2 * n + i];
  return d;
}

poker::Action sample_bot(const std::vector<PolicyAction>& dist, bs::SplitMix64& rng) {
  double total = 0.0;
  for (const PolicyAction& pa : dist) {
    if (!std::isfinite(pa.probability) || pa.probability < 0.0)
      throw std::runtime_error("practice bot returned a non-finite/negative probability");
    total += pa.probability;
  }
  if (std::abs(total - 1.0) > 1e-9)
    throw std::runtime_error("practice bot distribution does not sum to 1");
  return sample_distribution(dist, unit_draw(rng));
}

}  // namespace

std::unique_ptr<BehaviorPolicy> make_practice_bot(PracticeDifficulty difficulty) {
  switch (difficulty) {
    case PracticeDifficulty::Easy:
      return std::make_unique<UniformBehaviorPolicy>();
    case PracticeDifficulty::Medium:
      return std::make_unique<BaselineBehaviorPolicy>();
    case PracticeDifficulty::Engine:
      // The Engine tier leaves bot seats null; the caller injects a policy
      // via PracticeTable::set_bot().
      return nullptr;
  }
  throw std::invalid_argument("unknown practice difficulty");
}

struct PracticeTable::Impl {
  PracticeConfig cfg;
  HumanAgent human;
  bs::SplitMix64 rng;
  std::size_t button = 0;
  std::size_t hands = 0;
  std::array<double, 10> session{};
  std::array<int, 5> board{};
  std::size_t revealed = 0;
  std::vector<HoleCards> holes;
  // One bot policy per non-human seat, indexed by logical seat (human slot is
  // null).
  std::vector<std::unique_ptr<BehaviorPolicy>> bots;

  Impl(PracticeConfig c, HumanAgent h) : cfg(std::move(c)), human(std::move(h)), rng(seed_of(cfg)) {
    if (cfg.seats < 2 || cfg.seats > 10)
      throw std::invalid_argument("practice table requires 2..10 seats");
    if (cfg.human_seat >= cfg.seats)
      throw std::invalid_argument("human seat is not seated");
    if (!human)
      throw std::invalid_argument("practice table requires a human agent");
    if (cfg.big_blind < 2)
      throw std::invalid_argument("big blind must be at least 2 so the small blind is nonzero");
    if (cfg.starting_stack < cfg.big_blind)
      throw std::invalid_argument("starting stack must cover the big blind");
    bots.clear();
    bots.resize(cfg.seats);
    for (std::size_t s = 0; s < cfg.seats; ++s)
      if (s != cfg.human_seat)
        bots[s] = make_practice_bot(cfg.difficulty);
  }

  static std::uint64_t seed_of(const PracticeConfig& c) {
    return c.rng_seed ? c.rng_seed : 0x7072616374696365ULL;
  }
};

PracticeTable::PracticeTable(PracticeConfig config, HumanAgent human)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(human))) {}

PracticeTable::~PracticeTable() = default;

std::size_t PracticeTable::hands_played() const noexcept {
  return impl_->hands;
}
std::size_t PracticeTable::seats() const noexcept {
  return impl_->cfg.seats;
}
std::size_t PracticeTable::human_seat() const noexcept {
  return impl_->cfg.human_seat;
}
poker::Chips PracticeTable::big_blind() const noexcept {
  return impl_->cfg.big_blind;
}
std::size_t PracticeTable::button() const noexcept {
  return impl_->button;
}
double PracticeTable::session_result(std::size_t seat) const {
  if (seat >= impl_->cfg.seats)
    throw std::out_of_range("practice table session seat is outside the table");
  return impl_->session[seat];
}
const PracticeConfig& PracticeTable::config() const noexcept {
  return impl_->cfg;
}
std::vector<int> PracticeTable::revealed_board() const {
  std::vector<int> out;
  const std::size_t count = std::min<std::size_t>(impl_->revealed, 5);
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i)
    out.push_back(impl_->board[i]);
  return out;
}

void PracticeTable::set_bot(std::size_t seat, std::unique_ptr<BehaviorPolicy> bot) {
  if (seat >= impl_->cfg.seats)
    throw std::invalid_argument("practice table: set_bot seat is outside the table");
  if (seat == impl_->cfg.human_seat)
    throw std::invalid_argument("practice table: set_bot cannot replace the human seat");
  if (!bot)
    throw std::invalid_argument("practice table: set_bot requires a non-null bot");
  impl_->bots[seat] = std::move(bot);
}

std::array<double, 10> PracticeTable::play_hand(PracticeObserver* observer) {
  const std::size_t n = impl_->cfg.seats;
  const std::size_t human = impl_->cfg.human_seat;
  const std::uint64_t hand_id = impl_->hands;

  const poker::GameDef def = make_def(impl_->cfg, impl_->button);
  const Deal deal = deal_cards(n, impl_->rng);
  impl_->holes = deal.holes;
  impl_->board = deal.board;
  impl_->revealed = 0;

  if (observer)
    observer->on_hand_start(poker::GameState(def), hand_id, deal.holes[human]);

  poker::GameState state(def);
  HandLog log;
  std::uint64_t decisions = 0;
  bool folded = false;
  std::array<double, 10> utility{};

  while (true) {
    switch (state.phase()) {
      case poker::Phase::Action: {
        const std::size_t seat = *state.actor();
        const poker::LegalActions legal = state.legal();
        poker::Action chosen;
        std::vector<PolicyAction> observed;
        const bool is_human = seat == human;
        if (is_human) {
          chosen = impl_->human(state, seat, deal.holes[seat], *this);
        } else {
          if (!impl_->bots[seat])
            throw std::runtime_error(
                "practice table: Engine tier requires set_bot before play_hand");
          PolicyContext ctx;
          ctx.hand_log = &log;
          ctx.decision_seed = decision_seed(hand_id, decisions);
          observed = impl_->bots[seat]->distribution(state, seat, deal.holes[seat], ctx);
          chosen = sample_bot(observed, impl_->rng);
        }
        ++decisions;
        if (!legal.contains(chosen))
          throw std::runtime_error(is_human ? "practice table: human selected an illegal action"
                                            : "practice table: bot selected an illegal action");
        if (observer)
          observer->on_decision(state, seat, is_human, observed, chosen);
        street_log(log, state.street()).push_back(LoggedAction{seat, chosen});
        state = state.after_action(seat, chosen);
        break;
      }
      case poker::Phase::Deal: {
        const std::size_t want = state.board().size();  // next board index
        if (want >= 5)
          throw std::runtime_error("practice table: deal phase with a full board");
        state = state.after_card(deal.board[want]);
        impl_->revealed = state.board().size();
        // The engine reveals the flop through three one-card Deal phases;
        // notify observers only at the completed flop (3), turn (4), river (5).
        if (observer && (impl_->revealed == 3 || impl_->revealed == 4 || impl_->revealed == 5))
          observer->on_board(state);
        break;
      }
      case poker::Phase::Folded: {
        const poker::ContributionSettlement s = state.settle_fold();
        folded = true;
        long long sum = 0;
        for (std::size_t i = 0; i < n; ++i) {
          utility[i] = static_cast<double>(s.chip_utility[i]);
          sum += s.chip_utility[i];
        }
        if (sum != 0)
          throw std::runtime_error("practice table fold settlement is not zero-sum");
        goto done;
      }
      case poker::Phase::Showdown: {
        std::vector<std::array<int, 2>> live_holes;
        live_holes.reserve(state.live_players().size());
        for (std::size_t s : state.live_players())
          live_holes.push_back(deal.holes[s]);
        const poker::ContributionSettlement s = state.settle_showdown(live_holes);
        long long sum = 0;
        for (std::size_t i = 0; i < n; ++i) {
          utility[i] = static_cast<double>(s.chip_utility[i]);
          sum += s.chip_utility[i];
        }
        if (sum != 0)
          throw std::runtime_error("practice table showdown settlement is not zero-sum");
        goto done;
      }
    }
  }

done:
  for (std::size_t i = 0; i < n; ++i)
    impl_->session[i] += utility[i];
  // Only a showdown reveals cards: the live players that reached it. Hands won
  // by a fold expose nobody's hole cards.
  std::vector<std::pair<std::size_t, HoleCards>> shown;
  if (!folded)
    for (std::size_t s : state.live_players())
      shown.emplace_back(s, deal.holes[s]);
  if (observer)
    observer->on_hand_end(state, utility, folded, hand_id, shown);
  ++impl_->hands;
  impl_->button = (impl_->button + 1) % n;
  return utility;
}

}  // namespace bs::stage6
