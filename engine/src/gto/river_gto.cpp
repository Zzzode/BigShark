#include <bs/eval.hpp>
#include <bs/river_gto.hpp>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

#include "gto/cfr_solver.h"
#include "gto/range_builder.h"
#include "gto/range_tracker.h"
#include "gto/river_solution_data.h"

#if defined(BSGTO_HAVE_HIGHS)
#include <algorithm>
#include <memory>

#include "gto/highs_backend.h"
#include "gto/river_game.h"
#include "gto/sf_lp.h"
#include "open_spiel/algorithms/infostate_tree.h"
#include "open_spiel/spiel.h"

namespace bs {
namespace gto {
namespace internal_lp = ::bigshark;
namespace osp = ::open_spiel;
namespace osalg = ::open_spiel::algorithms;
}  // namespace gto
}  // namespace bs
#endif

namespace bs::gto {

RiverSolution::RiverSolution() : data_(std::make_unique<RiverSolutionData>()) {}
RiverSolution::~RiverSolution() = default;
RiverSolution::RiverSolution(RiverSolution&&) noexcept = default;
RiverSolution& RiverSolution::operator=(RiverSolution&&) noexcept = default;

RiverSolveResult RiverSolution::result() const {
  return data_->meta;
}

int RiverSolution::NumActions(RiverNode node) {
  return (node == RiverNode::kFaceBet || node == RiverNode::kFaceLead) ? 3 : 2;
}

bool RiverSolution::ProbsFor(int combo, RiverNode node, double* out) const {
  const NodePolicy& m = data_->node[(int)node];
  auto it = m.find(combo);
  if (it == m.end())
    return false;
  const int na = NumActions(node);
  for (int a = 0; a < na; ++a)
    out[a] = it->second[a];
  return true;
}

int RiverSolution::ChooseAction(int combo, RiverNode node, uint64_t seed) const {
  std::array<double, 3> p{};
  const int na = NumActions(node);
  if (!ProbsFor(combo, node, p.data()))
    return -1;
  // deterministic uniform draw from seed (splitmix64)
  uint64_t z = seed + 0x9E3779B97F4A7C15ULL;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  z ^= z >> 31;
  const double u = (z >> 11) * (1.0 / 9007199254740992.0);
  double acc = 0;
  for (int a = 0; a < na; ++a) {
    acc += p[a];
    if (u < acc)
      return a;
  }
  return na - 1;
}

namespace {
#if defined(BSGTO_HAVE_HIGHS)
RiverNode MapNode(int player, const std::string& h) {
  if (player == 0) {
    if (h == "")
      return RiverNode::kRoot;
    if (h == "cb")
      return RiverNode::kFaceLead;
    return RiverNode::kFaceRaiseI;  // "br"
  }
  if (h == "c")
    return RiverNode::kAfterC;
  if (h == "b")
    return RiverNode::kFaceBet;
  return RiverNode::kFaceRaiseO;  // "cbr"
}

// Solve both players' exact LPs and fill all six node policies. Returns false on
// any solver failure so the caller drops to bounded CFR.
bool SolveExact(const std::vector<int>& board, int pot, float bf, float rf, const Range& ip,
                const Range& oop, RiverSolutionData& out) {
  using internal_lp::BuildSequenceFormLp;
  using internal_lp::EnumerateRiverDeals;
  using internal_lp::ExtractPolicy;
  using internal_lp::HighsSolveResult;
  using internal_lp::Infopolicy;
  using internal_lp::MakeRiverGame;
  using internal_lp::SequenceFormLp;
  using internal_lp::SolveLpHighs;
  auto game = MakeRiverGame(board, pot, bf, rf);
  auto deals = EnumerateRiverDeals(*game, ip, oop);
  if (deals.empty())
    return false;
  auto root = game->NewInitialState();
  std::vector<std::shared_ptr<const osp::State>> states;
  std::vector<const osp::State*> ptrs;
  std::vector<double> probs;
  states.reserve(deals.size());
  ptrs.reserve(deals.size());
  probs.reserve(deals.size());
  for (const auto& d : deals) {
    auto s = root->Child(d.chance_action);
    ptrs.push_back(s.get());
    probs.push_back(d.prob);
    states.push_back(std::move(s));
  }
  auto obs = game->MakeObserver(osp::kInfoStateObsType, {});
  auto t0 = osalg::MakeInfostateTree(ptrs, probs, obs, 0, false, 1000);
  auto t1 = osalg::MakeInfostateTree(ptrs, probs, obs, 1, false, 1000);
  SequenceFormLp lp0 = BuildSequenceFormLp(t0, t1, 0);
  SequenceFormLp lp1 = BuildSequenceFormLp(t0, t1, 1);
  HighsSolveResult r0 = SolveLpHighs(lp0.model);
  HighsSolveResult r1 = SolveLpHighs(lp1.model);
  if (!r0.ok || !r1.ok)
    return false;
  std::vector<Infopolicy> p0 = ExtractPolicy(lp0, r0.primal);
  std::vector<Infopolicy> p1 = ExtractPolicy(lp1, r1.primal);
  auto ingest = [&](const std::vector<Infopolicy>& pol) {
    for (const Infopolicy& p : pol) {
      auto b1 = p.infostate.find('|');
      auto b2 = p.infostate.find('|', b1 + 1);
      int player = std::stoi(p.infostate.substr(0, b1));
      int combo = std::stoi(p.infostate.substr(b1 + 1, b2 - b1 - 1));
      std::string h = p.infostate.substr(b2 + 1);
      RiverNode node = MapNode(player, h);
      NodePolicy& m = out.node[(int)node];
      std::array<double, 3> q{0, 0, 0};
      for (size_t a = 0; a < p.probs.size(); ++a)
        q[a] = p.probs[a];
      m[combo] = q;
    }
  };
  ingest(p0);
  ingest(p1);
  out.meta.ok = true;
  out.meta.exact = true;
  out.meta.value_to_ip = -r0.objective;
  out.meta.exploitability_pot = 0.0;  // exact equilibrium; refined below is noise
  return true;
}
#endif

// ---- small process-wide solution cache (same board/lines/ranges -> reuse) ----
struct Cache {
  std::mutex mu;
  std::unordered_map<std::string, std::shared_ptr<RiverSolutionData>> map;
  std::vector<std::string> order;
  static constexpr size_t kMax = 6;
};
Cache& cache() {
  static Cache c;
  return c;
}
}  // namespace

std::unique_ptr<RiverSolution> SolveRiver(std::vector<int> board, int pot, float bet_frac,
                                          float raise_frac, const Range& ip_range,
                                          const Range& oop_range, const RiverSolveOptions& opts) {
  // cache key: board + sizes + range fingerprints
  std::string key;
  {
    std::ostringstream os;
    os << (opts.allow_exact ? 'E' : 'C') << '|';
    for (int b : board)
      os << b << '.';
    os << '|' << pot << '|' << bet_frac << '|' << raise_frac << '|';
    auto fp = [&](const Range& r) {
      uint64_t h = 1469598103934665603ULL;
      for (int c = 0; c < N_COMBOS; ++c)
        if (r[c] > 0) {
          h ^= (uint32_t)c;
          h *= 1099511628211ULL;
        }
      os << h << ';';
    };
    fp(ip_range);
    fp(oop_range);
    key = os.str();
  }
  Cache& cc = cache();
  {
    std::lock_guard<std::mutex> lk(cc.mu);
    auto it = cc.map.find(key);
    if (it != cc.map.end()) {
      auto sol = std::make_unique<RiverSolution>();
      sol->data_ = it->second;  // share immutable equilibrium
      return sol;
    }
  }

  auto data = std::make_shared<RiverSolutionData>();
  bool exact_ok = false;

  // Estimate the joint deal count cheaply to schedule within the time budget.
  int nip = 0, noop = 0;
  for (int c = 0; c < N_COMBOS; ++c) {
    if (ip_range[c] > 0.0f)
      ++nip;
    if (oop_range[c] > 0.0f)
      ++noop;
  }
  // ~8% of ordered pairs share a card; exact LP (two solves) measured at about
  // 2*(15ms + 0.10ms*deals) on this hardware class.
  const double est_deals = 0.92 * nip * noop;
#if defined(BSGTO_HAVE_HIGHS)
  const double est_lp_ms = 2.0 * (15.0 + 0.10 * est_deals);
  const bool exact_fits = est_lp_ms <= opts.time_budget_s * 1000.0 * 0.55;
  if (opts.allow_exact && exact_fits)
    exact_ok = SolveExact(board, pot, bet_frac, raise_frac, ip_range, oop_range, *data);
#endif
  if (!exact_ok) {
    CfrOptions co;
    // DCFR+ reaches <~1% pot by a few thousand iterations on these trees; size
    // iterations to the budget using ~0.49us per joint deal per iteration, then
    // hard-stop on the wall clock as a second guard.
    const double ms_per_iter = 0.00049 * est_deals;
    int adaptive = ms_per_iter > 0 ? (int)(opts.time_budget_s * 1000.0 * 0.8 / ms_per_iter)
                                   : opts.cfr_iterations;
    co.iterations = std::max(500, std::min(opts.cfr_iterations, adaptive));
    co.time_budget_s = opts.time_budget_s * 0.85;
    CfrEquilibrium e = SolveCfr(board, pot, bet_frac, raise_frac, ip_range, oop_range, co);
    data->meta.ok = e.ok;
    data->meta.exact = false;
    data->meta.value_to_ip = e.value_to_ip;
    data->meta.exploitability_pot = e.exploitability_pot;
    data->node = e.node;
  }

  {
    std::lock_guard<std::mutex> lk(cc.mu);
    if (cc.map.size() >= Cache::kMax && !cc.order.empty()) {
      cc.map.erase(cc.order.front());
      cc.order.erase(cc.order.begin());
    }
    cc.map[key] = data;
    cc.order.push_back(key);
  }
  auto sol = std::make_unique<RiverSolution>();
  sol->data_ = data;
  return sol;
}

Range BuildRiverRange(const std::vector<int>& board, int cap) {
  return BuildRiverRange(board, static_cast<const std::vector<std::string>*>(nullptr), cap);
}

TrackedRanges TrackRiverRanges(const std::vector<int>& board, int hero_combo, bool hero_is_ip,
                               const std::string& flop_line, const std::string& turn_line,
                               const TrackedRangesOptions& opt) {
  TrackOptionsImpl o;
  o.cap = opt.cap;
  o.preflop_raises = opt.preflop_raises;
  o.hero_was_aggressor = opt.hero_was_aggressor;
  TrackedRangesImpl tr =
      TrackRiverRangesImpl(board, hero_combo, hero_is_ip, flop_line, turn_line, o);
  TrackedRanges out;
  out.ip = tr.ip;
  out.oop = tr.oop;
  return out;
}

}  // namespace bs::gto
