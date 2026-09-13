#include "gto/river_game.h"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "bs/eval.hpp"
#include "bs/range.hpp"

namespace bigshark {
using open_spiel::Action;
using open_spiel::Player;
namespace G = open_spiel;

namespace {
bool IsLeaf(const std::string& h) {
  return h == "cc" || h == "bf" || h == "bc" || h == "cbf" || h == "cbc" || h == "brf" ||
         h == "brc" || h == "cbrf" || h == "cbrc";
}
int Actor(const std::string& h) {
  return (h == "" || h == "cb" || h == "br") ? 0 : 1;
}
const std::vector<const char*>& Kids(const std::string& h) {
  static const std::vector<const char*> root = {"c", "b"};
  static const std::vector<const char*> afterc = {"cc", "cb"};
  static const std::vector<const char*> afterb = {"bf", "bc", "br"};
  static const std::vector<const char*> cb = {"cbf", "cbc", "cbr"};
  static const std::vector<const char*> br = {"brf", "brc"};
  static const std::vector<const char*> cbr = {"cbrf", "cbrc"};
  if (h == "")
    return root;
  if (h == "c")
    return afterc;
  if (h == "b")
    return afterb;
  if (h == "cb")
    return cb;
  if (h == "br")
    return br;
  return cbr;
}
bool ValidPair(int i, int j, const std::vector<int>& board) {
  auto [a1, b1] = bs::comboTable()[i];
  auto [a2, b2] = bs::comboTable()[j];
  if (a1 == a2 || a1 == b2 || b1 == a2 || b1 == b2)
    return false;
  for (int c : board)
    if (c == a1 || c == b1 || c == a2 || c == b2)
      return false;
  return true;
}
}  // namespace

// ---- Game ----
static G::GameType MakeGameType() {
  return G::GameType{"bigshark_river",
                     "BigShark River",
                     G::GameType::Dynamics::kSequential,
                     G::GameType::ChanceMode::kExplicitStochastic,
                     G::GameType::Information::kImperfectInformation,
                     G::GameType::Utility::kZeroSum,
                     G::GameType::RewardModel::kTerminal,
                     2,
                     2,
                     true,
                     false,
                     true,
                     false,
                     {}};
}
RiverGame::RiverGame(std::vector<int> board, int pot, float betFrac, float raiseFrac)
    : G::Game(MakeGameType(), {}),
      board_(std::move(board)),
      pot_(pot),
      betFrac_(betFrac),
      raiseFrac_(raiseFrac),
      deals_(MakeRiverDealTable(board_)) {}
double RiverGame::MinUtility() const {
  return -(pot_ / 2.0 + raiseFrac_ * (pot_ + 2 * betFrac_ * pot_));
}
double RiverGame::MaxUtility() const {
  return (pot_ / 2.0 + raiseFrac_ * (pot_ + 2 * betFrac_ * pot_));
}

std::unique_ptr<G::State> RiverGame::NewInitialState() const {
  return std::make_unique<RiverState>(std::static_pointer_cast<const G::Game>(shared_from_this()),
                                      board_, pot_, betFrac_, raiseFrac_, deals_);
}

// ---- State ----
RiverState::RiverState(std::shared_ptr<const G::Game> game, const std::vector<int>& board, int pot,
                       float betFrac, float raiseFrac, std::shared_ptr<const RiverDealTable> deals)
    : G::State(std::move(game)),
      board_(board),
      pot_(pot),
      betFrac_(betFrac),
      raiseFrac_(raiseFrac),
      deals_(std::move(deals)) {}

Player RiverState::CurrentPlayer() const {
  if (!cardsDealt_)
    return G::kChancePlayerId;
  return IsLeaf(h_) ? G::kTerminalPlayerId : Actor(h_);
}
bool RiverState::IsTerminal() const {
  return cardsDealt_ && IsLeaf(h_);
}

int RiverState::Winner() const {
  int c7[7], n;
  auto [c0, c1] = bs::comboTable()[ipCombo_];
  auto [d0, d1] = bs::comboTable()[oopCombo_];
  n = 0;
  c7[n++] = c0;
  c7[n++] = c1;
  for (int x : board_)
    c7[n++] = x;
  unsigned int sa = bs::evaluate(c7, n).score;
  c7[0] = d0;
  c7[1] = d1;
  unsigned int sb = bs::evaluate(c7, n).score;
  return sa > sb ? 0 : (sa < sb ? 1 : 2);
}
double RiverState::PayoffIP() const {
  const double a = pot_ / 2.0, B = betFrac_ * pot_, R = raiseFrac_ * (pot_ + 2 * B);
  const int w = Winner();
  auto sd = [&](double s) { return w == 0 ? s : (w == 2 ? 0.0 : -s); };
  if (h_ == "cc")
    return sd(a);
  if (h_ == "bf")
    return a;
  if (h_ == "bc")
    return sd(a + B);
  if (h_ == "cbf")
    return -a;
  if (h_ == "cbc")
    return sd(a + B);
  if (h_ == "brf")
    return -(a + B);
  if (h_ == "brc")
    return sd(a + R);
  if (h_ == "cbrf")
    return a + B;
  if (h_ == "cbrc")
    return sd(a + R);
  return 0;
}
std::vector<double> RiverState::Returns() const {
  double u = PayoffIP();
  return {u, -u};
}

std::vector<Action> RiverState::LegalActions() const {
  std::vector<Action> v;
  if (!cardsDealt_ || IsLeaf(h_))
    return v;
  for (Action k = 0; k < (Action)Kids(h_).size(); k++)
    v.push_back(k);
  return v;
}
std::vector<std::pair<Action, double>> RiverState::ChanceOutcomes() const {
  std::vector<std::pair<Action, double>> out;
  const size_t n = deals_ ? deals_->size() : 0;
  out.reserve(n);
  const double p = n ? 1.0 / (double)n : 0.0;
  for (size_t idx = 0; idx < n; idx++)
    out.push_back({(Action)idx, p});
  return out;
}
void RiverState::DoApplyAction(Action action) {
  if (!cardsDealt_) {
    const auto [i, j] = (*deals_)[(size_t)action];
    ipCombo_ = i;
    oopCombo_ = j;
    cardsDealt_ = true;
    return;
  }
  h_ = Kids(h_)[action];
}
std::string RiverState::ActionToString(Player, Action) const {
  return h_;
}
std::string RiverState::ToString() const {
  return absl::StrCat("river h=", h_, " ip=", ipCombo_, " oop=", oopCombo_);
}
std::string RiverState::InformationStateString(Player player) const {
  int combo = player == 0 ? ipCombo_ : oopCombo_;
  return absl::StrCat(player, "|", combo, "|", h_);
}
std::string RiverState::ObservationString(Player p) const {
  return InformationStateString(p);
}
std::unique_ptr<G::State> RiverState::Clone() const {
  return std::make_unique<RiverState>(*this);
}

std::shared_ptr<const RiverGame> MakeRiverGame(const std::vector<int>& board, int pot,
                                               float betFrac, float raiseFrac) {
  return std::make_shared<RiverGame>(board, pot, betFrac, raiseFrac);
}

std::shared_ptr<const RiverDealTable> MakeRiverDealTable(const std::vector<int>& board) {
  // Process-wide memoization: a river board produces the same ~1M-entry table
  // regardless of ranges/bet sizes, so reuse it across solves (bounded LRU).
  static std::mutex mu;
  static std::vector<std::pair<std::vector<int>, std::shared_ptr<const RiverDealTable>>> cache;
  static constexpr size_t kMax = 8;
  std::vector<int> key = board;
  std::sort(key.begin(), key.end());
  {
    std::lock_guard<std::mutex> lk(mu);
    for (auto& [b, t] : cache)
      if (b == key) {
        if (&b != &cache.front().first) { /* touch: move to front */
        }
        return t;
      }
  }
  auto table = std::make_shared<RiverDealTable>();
  for (int i = 0; i < bs::N_COMBOS; ++i)
    for (int j = 0; j < bs::N_COMBOS; ++j)
      if (ValidPair(i, j, board))
        table->push_back({i, j});
  {
    std::lock_guard<std::mutex> lk(mu);
    if (cache.size() >= kMax)
      cache.erase(cache.begin());
    cache.push_back({key, table});
  }
  return table;
}

std::vector<RiverDeal> EnumerateRiverDeals(const RiverGame& game, const bs::Range& w_ip,
                                           const bs::Range& w_oop) {
  std::vector<RiverDeal> out;
  double total = 0.0;
  const RiverDealTable& t = *game.Deals();
  out.reserve(t.size());
  for (size_t a = 0; a < t.size(); ++a) {
    const auto [i, j] = t[a];
    const double w = double(w_ip[i]) * double(w_oop[j]);
    if (w > 0.0f) {
      out.push_back({(open_spiel::Action)a, i, j, w});
      total += w;
    }
  }
  for (auto& d : out)
    d.prob /= total;
  return out;
}
}  // namespace bigshark
