// RFC 0047 G3 step-2 perturbation probe (measurement-only, manual, NOT a
// CTest).
//
// Injects known additive continuation-value errors into every unique river
// leaf matrix, re-solves the two-street turn game under the perturbed leaves,
// and evaluates the resulting fixed turn strategy against the TRUE leaves with
// the independent telescoping exact-best-response walker. For every injection
// it records:
//   e_max  = max_(seat,combo) |delta| / leaf pot p,
//   e_l2   = sqrt( sum_(leaf,seat,combo) rho(seat,combo) (delta/p)^2 )
//            with rho the exported turn-equilibrium counterfactual reach,
//   hybrid NashConv BB (turn gap vs true leaves + reach-weighted river term),
// and the zero-perturbation telescoping identity (walker turn gap == the turn
// solver's own NashConv).
//
// Arms (deterministic, seedless):
//   zm  zero-mean alternating +/-, recentered to exact mean 0 per seat;
//   coh coherent same-sign shift of every one of ONE seat's combos;
//   pat structured sign pattern (combo index modulo 7), zero-mean recentered;
//   act the real signed per-leaf G2 drift shape (candidate-minus-reference
//       root CV at RIVER_ITERS vs REF_ITERS, normalized to max|.|=1).
// Levels are additive amplitudes as fractions of each leaf pot:
//   1e-6, 1e-5, 1e-4, 1e-3, 1e-2, 2e-2 (the last brackets e_max_frozen).
//
//   bigshark-rfc0047-g3-probe STRATUM RIVER_ITERS TURN_ITERS [THREADS]
//                             [REF_ITERS]
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "frontier_enumerator.hpp"
#include "river_solver.hpp"
#include "strata.hpp"
#include "turn_solver.hpp"

using namespace bs::rfc0047;

namespace {

struct Key {
  std::array<std::uint64_t, 5> ledger{};
  std::array<int, 5> board5{};
  bool operator==(const Key& o) const { return ledger == o.ledger && board5 == o.board5; }
};
struct KeyHash {
  std::size_t operator()(const Key& k) const noexcept {
    std::size_t h = 1469598103934665603ULL;
    for (std::uint64_t x : k.ledger) {
      h ^= static_cast<std::size_t>(x);
      h *= 1099511628211ULL;
    }
    for (int c : k.board5) {
      h ^= static_cast<std::size_t>(static_cast<unsigned>(c));
      h *= 1099511628211ULL;
    }
    return h;
  }
};

struct Group {
  Key key;
  double pot = 0.0;
  std::vector<double> matrix;   // true (unperturbed) seat-0 values
  double river_nc_chips = 0.0;  // river subgame summed NashConv
  // "act" arm: signed per-combo candidate-minus-reference root-CV drift
  // (real G2 error shape), per seat, normalized to max|.| = 1.
  std::array<std::vector<double>, 2> act_unit;
  // Un-normalized signed drift/pot (for the reach-weighted real L* norm).
  std::array<std::vector<double>, 2> drift;
};

// Additive amplitudes as fractions of each leaf pot. The upper level 2e-2
// brackets the production operating point e_max_frozen ~= 1.84e-2 (the worst
// shipped G2 per-entry bound, an 8-chip-pot single combo), per the frozen
// "span 1e-6 to the gate scale" requirement.
constexpr double kLevels[] = {1e-6, 1e-5, 1e-4, 1e-3, 1e-2, 2e-2};

// Per-seat signed unit pattern in {-1,+1}, optionally zero-mean recentered.
double pattern_sign(const char* arm, std::uint32_t c) {
  if (arm[0] == 'z')  // zm: alternating
    return (c & 1u) ? -1.0 : 1.0;
  if (arm[0] == 'c')  // coh: coherent same sign
    return 1.0;
  // pat: structured modulo-7 sign
  return (c % 7u) < 4u ? 1.0 : -1.0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr,
                 "usage: %s STRATUM RIVER_ITERS TURN_ITERS [THREADS] "
                 "[REF_ITERS] [OUT_CSV]\n",
                 argv[0]);
    return 2;
  }
  const StratumSpec* spec = find_stratum(argv[1]);
  if (spec == nullptr) {
    std::fprintf(stderr, "unknown stratum '%s'\n", argv[1]);
    return 2;
  }
  const std::uint64_t river_iters = std::stoull(argv[2]);
  const std::uint64_t turn_iters = std::stoull(argv[3]);
  unsigned hw = std::thread::hardware_concurrency();
  const std::size_t n_threads = argc >= 5 ? std::stoul(argv[4]) : (hw ? hw : 1);
  // High-accuracy reference solve supplies the signed actual-error pattern.
  const std::uint64_t ref_iters = argc >= 6 ? std::stoull(argv[5]) : river_iters * 4;
  const char* out_csv = argc >= 7 ? argv[6] : nullptr;
  const std::string arm_set = argc >= 8 ? argv[7] : "all";
  const char* norms_csv = argc >= 9 ? argv[8] : nullptr;
  // Append so one campaign can concatenate all strata into a single frame;
  // write the header only when the file is newly created/empty.
  FILE* csv = nullptr;
  if (out_csv != nullptr) {
    csv = std::fopen(out_csv, "a+");
    if (csv == nullptr) {
      std::fprintf(stderr, "cannot open '%s' for append\n", out_csv);
      return 2;
    }
    if (std::ftell(csv) == 0)
      std::fprintf(csv,
                   "stratum,arm,level,e_max,e_l2,turn_gap_bb,"
                   "river_term_bb,delta_hybrid_bb,analytic_bound_bb\n");
  }

  const std::vector<FrontierLeaf> leaves = enumerate_frontiers(*spec, 1);

  std::unordered_map<Key, std::size_t, KeyHash> group_of;
  std::vector<std::size_t> group_of_leaf(leaves.size());
  for (std::size_t i = 0; i < leaves.size(); ++i) {
    const auto& d = leaves[i].river_def;
    Key k{{d.pot, d.contributions[0], d.contributions[1], d.stacks[0], d.stacks[1]},
          leaves[i].board5};
    auto [it, inserted] = group_of.emplace(k, group_of.size());
    group_of_leaf[i] = it->second;
  }
  const std::size_t n_groups = group_of.size();
  std::vector<Group> groups(n_groups);
  for (auto& [k, idx] : group_of)
    groups[idx].key = k;

  const auto wall0 = std::chrono::steady_clock::now();
  std::atomic<std::size_t> next{0}, done{0};
  std::mutex log;
  std::exception_ptr err;
  auto worker = [&]() {
    try {
      for (;;) {
        const std::size_t g = next.fetch_add(1);
        if (g >= n_groups || err)
          return;
        const FrontierLeaf* rep = nullptr;
        for (std::size_t i = 0; i < leaves.size(); ++i)
          if (group_of_leaf[i] == g) {
            rep = &leaves[i];
            break;
          }
        RiverSolveConfig cfg;
        cfg.def = rep->river_def;
        cfg.iterations = river_iters;
        cfg.linear_averaging = true;
        cfg.compute_deal_matrix = true;
        const RiverSolveResult r = solve_river_subgame(cfg, {});
        groups[g].matrix = std::move(r.deal_value_seat0);
        groups[g].pot = static_cast<double>(rep->river_def.pot);
        groups[g].river_nc_chips = r.nash_conv_chips;

        // Signed candidate-minus-reference per-combo CV drift: the real G2
        // error shape (not fabs), normalized by pot and rescaled to max|.|=1
        // so the arm's amplitude is set purely by the injected level. The
        // production spike-only mode ("prod") needs no reference/drift.
        for (int seat = 0; seat < 2; ++seat) {
          groups[g].drift[seat].assign(kRiverC, 0.0);
          groups[g].act_unit[seat].assign(kRiverC, 0.0);
        }
        // "full" needs the reference (real drift/L*); prod/prodfine skip it.
        if (arm_set != "prod" && arm_set != "prodfine") {
          // corner/full/norms/spike/all/patterns all keep the reference solve.
          RiverSolveConfig rcfg = cfg;
          rcfg.iterations = ref_iters;
          rcfg.compute_deal_matrix = false;
          const RiverSolveResult ref = solve_river_subgame(rcfg, {});
          const std::array<const std::vector<double>*, 2> rc = {&r.root_cfv_seat0,
                                                                &r.root_cfv_seat1};
          const std::array<const std::vector<double>*, 2> rr = {&ref.root_cfv_seat0,
                                                                &ref.root_cfv_seat1};
          for (int seat = 0; seat < 2; ++seat) {
            double mx = 0.0;
            for (std::uint32_t c = 0; c < kRiverC; ++c) {
              const double d = ((*rc[seat])[c] - (*rr[seat])[c]) / groups[g].pot;
              groups[g].drift[seat][c] = d;
              groups[g].act_unit[seat][c] = d;
              mx = std::max(mx, std::fabs(d));
            }
            if (mx > 0.0)
              for (double& x : groups[g].act_unit[seat])
                x /= mx;
          }
        }
        const std::size_t d = done.fetch_add(1) + 1;
        if (d % 24 == 0 || d == n_groups) {
          std::lock_guard<std::mutex> lk(log);
          std::fprintf(stderr, "river leaves solved %zu/%zu\n", d, n_groups);
        }
      }
    } catch (...) {
      err = std::current_exception();
    }
  };
  std::vector<std::thread> pool;
  for (std::size_t t = 0; t < std::min(n_threads, n_groups); ++t)
    pool.emplace_back(worker);
  for (auto& th : pool)
    th.join();
  if (err)
    std::rethrow_exception(err);

  std::vector<double> river_nc(leaves.size());
  for (std::size_t i = 0; i < leaves.size(); ++i)
    river_nc[i] = groups[group_of_leaf[i]].river_nc_chips;

  auto fill_cfg = [&](TurnSolveConfig& tc, const std::vector<std::vector<double>*>& mats,
                      std::vector<TurnLeafValue>& tleaves) {
    tleaves.resize(leaves.size());
    for (std::size_t i = 0; i < leaves.size(); ++i)
      tleaves[i] = {leaves[i].child_node, leaves[i].board5, mats[i]->data()};
    tc.def = make_stratum_def(*spec, 1);
    tc.iterations = turn_iters;
    tc.linear_averaging = true;
    tc.threads = n_threads;
    tc.leaves = tleaves;
  };

  // One TurnLeafValue vector per config must outlive the config's span.
  std::vector<TurnLeafValue> true_tleaves;
  std::vector<std::vector<double>*> true_leaf_ptrs(leaves.size());
  for (std::size_t i = 0; i < leaves.size(); ++i)
    true_leaf_ptrs[i] = &groups[group_of_leaf[i]].matrix;
  TurnSolveConfig true_cfg;
  fill_cfg(true_cfg, true_leaf_ptrs, true_tleaves);

  std::fprintf(stderr, "solving unperturbed turn equilibrium...\n");
  const TurnSolveResult base = solve_turn_subgame(true_cfg);

  // Zero-perturbation telescoping identity.
  const HybridNashConv ident = turn_hybrid_nash_conv(true_cfg, base.sigma, river_nc);
  const double id_diff = std::fabs(ident.turn_gap_bb - base.nash_conv_bb);

  const double wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
  std::printf(
      "stratum=%s leaves=%zu groups=%zu river_iters=%llu "
      "turn_iters=%llu threads=%zu wall=%.1fs\n",
      spec->name, leaves.size(), n_groups, static_cast<unsigned long long>(river_iters),
      static_cast<unsigned long long>(turn_iters), n_threads, wall);
  std::printf("turn_action_nodes=%zu tree_size=%zu\n", base.turn_action_nodes, base.tree_size);
  std::printf(
      "baseline_turn_nc_bb=%.8f identity_walker_gap_bb=%.8f "
      "identity_diff_bb=%.3e river_term_bb=%.8f\n",
      base.nash_conv_bb, ident.turn_gap_bb, id_diff, ident.river_term_bb);
  // Identity row in the campaign frame: turn_gap is the zero-perturbation
  // baseline hybrid gap; the evaluator cross-checks it against solver NC.
  if (csv != nullptr) {
    std::fprintf(csv, "%s,identity,0,0,0,%.8f,%.8f,0,0\n", spec->name, ident.turn_gap_bb,
                 ident.river_term_bb);
    std::fflush(csv);
  }
  std::printf("arm,level,e_max,e_l2,turn_gap_bb,river_term_bb,delta_hybrid_bb\n");

  using Shifts = std::vector<std::array<std::vector<double>, 2>>;  // [g][seat][c]

  // Independent-walker BR-minus-sigma gap weights and matching equilibrium
  // rho, aggregated per group (the water-filling importance is w/rho).
  std::vector<TurnLeafGapWeight> gw_leaves = turn_leaf_gap_weights(true_cfg, base.sigma);
  Shifts gw(n_groups), grho(n_groups);
  for (std::size_t g = 0; g < n_groups; ++g)
    for (int seat = 0; seat < 2; ++seat) {
      gw[g][seat].assign(kRiverC, 0.0);
      grho[g][seat].assign(kRiverC, 0.0);
    }
  for (std::size_t li = 0; li < leaves.size(); ++li) {
    const std::size_t g = group_of_leaf[li];
    for (int seat = 0; seat < 2; ++seat)
      for (std::uint32_t c = 0; c < kRiverC; ++c) {
        gw[g][seat][c] += gw_leaves[li].w[seat][c];
        grho[g][seat][c] += base.leaf_reaches[li].rho[seat][c];
      }
  }
  const double bb_gate =
      true_cfg.def.big_blind > 0 ? static_cast<double>(true_cfg.def.big_blind) : 1.0;

  // Shape-free first-order box upper bound (independent review F1/F2):
  // maximize sum |w| pot |d| / BB over ALL pooled (group,seat,combo) cells
  // (global cross-group concentration, both w tails via sign(w)) subject to
  // |d|<=E and sum rho d^2 <= L^2. Optimal d is a capped water-fill ranked by
  // (|w| pot)/(rho); lambda is bisectioned to hit the energy.
  // All pooled cells with their (group,seat,combo) address, ranked by
  // (|w| pot/BB)/rho - the global box optimizer's ranking (R1/F1/F2).
  struct Cell {
    std::size_t g;
    int seat;
    std::uint32_t c;
    double coeff, rho;
  };
  auto build_cells = [&]() {
    std::vector<Cell> cells;
    for (std::size_t g = 0; g < n_groups; ++g)
      for (int seat = 0; seat < 2; ++seat)
        for (std::uint32_t c = 0; c < kRiverC; ++c) {
          const double rho = grho[g][seat][c];
          const double w = gw[g][seat][c];
          if (rho <= 0.0) {
            if (w != 0.0)
              throw std::runtime_error("analytic_box: zero-reach cell with nonzero gap weight");
            continue;
          }
          cells.push_back({g, seat, c, std::fabs(w) * groups[g].pot / bb_gate, rho});
        }
    std::sort(cells.begin(), cells.end(),
              [](const Cell& a, const Cell& b) { return a.coeff / a.rho > b.coeff / b.rho; });
    return cells;
  };
  const std::vector<Cell> cells = build_cells();
  auto d_at = [&](const Cell& x, double lam, double E) {
    double d = x.coeff / (lam * x.rho);
    return d > E ? E : d;
  };
  auto solve_lambda = [&](double E, double L) {
    auto energy = [&](double lam) {
      double s = 0.0;
      for (const Cell& x : cells) {
        const double d = d_at(x, lam, E);
        s += x.rho * d * d;
      }
      return s;
    };
    double lo = 1e-18, hi = 1e9;
    for (int it = 0; it < 200; ++it) {
      const double mid = 0.5 * (lo + hi);
      if (energy(mid) > L * L)
        lo = mid;
      else
        hi = mid;
    }
    return 0.5 * (lo + hi);
  };
  auto analytic_box = [&](double E, double L) {
    const double lam = solve_lambda(E, L);
    double bound = 0.0;
    for (const Cell& x : cells)
      bound += x.coeff * d_at(x, lam, E);
    return bound;
  };
  // R1: the exact box-corner maximizing shift d* (dimensionless pot fraction),
  // sign(w)-signed so both tails are exercised, written back per
  // (group,seat,combo). Injecting it and re-solving gives a measured delta that
  // must be <= analytic_box(E,L).
  auto corner_shifts = [&](double E, double L) {
    const double lam = solve_lambda(E, L);
    Shifts sh(n_groups);
    for (std::size_t g = 0; g < n_groups; ++g)
      for (int seat = 0; seat < 2; ++seat)
        sh[g][seat].assign(kRiverC, 0.0);
    for (const Cell& x : cells) {
      const double mag = d_at(x, lam, E);
      sh[x.g][x.seat][x.c] = (gw[x.g][x.seat][x.c] >= 0.0 ? mag : -mag);
    }
    return sh;
  };

  // Generic injection: dimensionless per-group per-seat shift sh (delta/pot),
  // amplitude level. Builds M' = M + p*level*(sh0[i] - sh1[j]), re-solves the
  // turn, evaluates the fixed perturbed strategy against the TRUE leaves, and
  // records e_max / reach-weighted e_l2 / hybrid delta plus the shape-free
  // analytic upper bound at the SAME (e_max,e_l2) (must dominate the measured
  // delta; the ratio is the first-order linearization quality check).
  auto inject = [&](const char* label, double level, const Shifts& sh) {
    std::vector<std::vector<double>> pert(n_groups);
    for (std::size_t g = 0; g < n_groups; ++g) {
      pert[g] = groups[g].matrix;
      const double amp = groups[g].pot * level;
      double* mp = pert[g].data();
      for (std::uint32_t i = 0; i < kRiverC; ++i) {
        const double e0 = amp * sh[g][0][i];
        for (std::uint32_t j = 0; j < kRiverC; ++j)
          mp[static_cast<std::size_t>(i) * kRiverC + j] += e0 - amp * sh[g][1][j];
      }
    }
    std::vector<std::vector<double>*> pert_ptrs(leaves.size());
    for (std::size_t i = 0; i < leaves.size(); ++i)
      pert_ptrs[i] = &pert[group_of_leaf[i]];
    std::vector<TurnLeafValue> pert_tleaves;
    TurnSolveConfig pcfg;
    fill_cfg(pcfg, pert_ptrs, pert_tleaves);
    const TurnSolveResult pr = solve_turn_subgame(pcfg);
    const HybridNashConv h = turn_hybrid_nash_conv(true_cfg, pr.sigma, river_nc);

    double e_max = 0.0, acc2 = 0.0;
    for (std::size_t li = 0; li < leaves.size(); ++li) {
      const std::size_t g = group_of_leaf[li];
      const TurnLeafReach& rho = base.leaf_reaches[li];
      for (int seat = 0; seat < 2; ++seat)
        for (std::uint32_t c = 0; c < kRiverC; ++c) {
          const double ef = level * sh[g][seat][c];
          e_max = std::max(e_max, std::fabs(ef));
          acc2 += rho.rho[seat][c] * ef * ef;
        }
    }
    const double e_l2 = std::sqrt(acc2);
    const double delta =
        (h.turn_gap_bb + h.river_term_bb) - (ident.turn_gap_bb + ident.river_term_bb);
    const double analytic = analytic_box(e_max, e_l2);
    std::printf("%s,%.0e,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f\n", label, level, e_max, e_l2, h.turn_gap_bb,
                h.river_term_bb, delta, analytic);
    if (csv != nullptr) {
      std::fprintf(csv, "%s,%s,%.0e,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f\n", spec->name, label, level,
                   e_max, e_l2, h.turn_gap_bb, h.river_term_bb, delta, analytic);
      std::fflush(csv);
    }
    std::fflush(stdout);
  };

  // Partially-coherent SPIKE: per group/seat rank combos by water-filling
  // importance w/rho among gap-increasing combos (w>0), and put a +level pot
  // payoff shift on the minimal set carrying a fraction q of their rho mass.
  auto spike_shifts = [&](double q) {
    Shifts sh(n_groups);
    for (std::size_t g = 0; g < n_groups; ++g) {
      for (int seat = 0; seat < 2; ++seat) {
        sh[g][seat].assign(kRiverC, 0.0);
        std::vector<std::uint32_t> cand;
        double total_rho = 0.0;
        for (std::uint32_t c = 0; c < kRiverC; ++c)
          if (gw[g][seat][c] > 0.0) {
            cand.push_back(c);
            total_rho += grho[g][seat][c];
          }
        std::sort(cand.begin(), cand.end(), [&](std::uint32_t a, std::uint32_t b) {
          const double ia = gw[g][seat][a] / (grho[g][seat][a] + 1e-30);
          const double ib = gw[g][seat][b] / (grho[g][seat][b] + 1e-30);
          return ia > ib;
        });
        const double target = q * total_rho;
        double acc = 0.0;
        for (std::uint32_t c : cand) {
          sh[g][seat][c] = 1.0;  // payoff-increasing for this seat
          acc += grho[g][seat][c];
          if (acc >= target)
            break;
        }
      }
    }
    return sh;
  };

  // Reach-weighted norm of the REAL (un-normalized) r600/r2400 drift: the
  // production-relevant e_l2 coordinate L* (Finding 2), and its max e_max.
  double emax_real = 0.0, l2star_acc = 0.0;
  for (std::size_t li = 0; li < leaves.size(); ++li) {
    const std::size_t g = group_of_leaf[li];
    const TurnLeafReach& rho = base.leaf_reaches[li];
    for (int seat = 0; seat < 2; ++seat)
      for (std::uint32_t c = 0; c < kRiverC; ++c) {
        const double d = groups[g].drift[seat][c];
        emax_real = std::max(emax_real, std::fabs(d));
        l2star_acc += rho.rho[seat][c] * d * d;
      }
  }
  const double lstar_real = std::sqrt(l2star_acc);

  // Shape-free box bound at the stratum's OWN measured full-census
  // (e_max_real, L*). Piecewise-linear in E (the capped cell set can change),
  // so no global-amplitude rescale is assumed; the evaluator uses this value
  // directly and reports E_env = max strata emax_real separately.
  const double box_own = analytic_box(emax_real, lstar_real);

  if (norms_csv != nullptr && arm_set != "prod" && arm_set != "prodfine") {
    // norms/full/corner write a fresh PER-STRATUM file (single writer -> no
    // append race); all/spike append to a shared sidecar.
    const char* mode =
        (arm_set == "norms" || arm_set == "full" || arm_set == "corner") ? "w" : "a+";
    FILE* nf = std::fopen(norms_csv, mode);
    if (nf != nullptr) {
      if (mode[0] == 'w' || std::ftell(nf) == 0)
        std::fprintf(nf, "stratum,emax_real,lstar_real,analytic_box_bb\n");
      std::fprintf(nf, "%s,%.8f,%.8f,%.8f\n", spec->name, emax_real, lstar_real, box_own);
      std::fclose(nf);
    }
  }
  std::fprintf(stderr, "real norms: e_max=%.6f L*=%.6f analytic_box=%.6f\n", emax_real, lstar_real,
               box_own);

  // norms-only: the production L* measurement needs the production river
  // solves and turn-equilibrium rho, but no perturbation injections.
  if (arm_set == "norms")
    return 0;

  // Precompute per-seat zero-mean offsets for the alternating/structured arms.
  auto recenter = [&](const char* arm) {
    double mean = 0.0;
    for (std::uint32_t c = 0; c < kRiverC; ++c)
      mean += pattern_sign(arm, c);
    mean /= static_cast<double>(kRiverC);
    return mean;
  };
  const double zm_mean = recenter("zm");
  const double pat_mean = recenter("pat");
  auto unit = [&](const char* arm, std::uint32_t c) {
    const double s = pattern_sign(arm, c);
    if (arm[0] == 'z')
      return s - zm_mean;
    if (arm[0] == 'p')
      return s - pat_mean;
    return s;
  };
  // Dimensionless delta/p for one (group, seat, combo): act uses the real
  // signed per-group G2 drift shape; coh shifts seat 0 only; zm/pat use the
  // global zero-mean pattern on both seats.
  auto uval = [&](const char* arm, std::size_t g, int seat, std::uint32_t c) -> double {
    if (arm[0] == 'a')
      return groups[g].act_unit[seat][c];
    if (arm[0] == 'c')
      return seat == 0 ? 1.0 : 0.0;
    return unit(arm, c);
  };

  auto pattern_shifts = [&](const char* arm) {
    Shifts sh(n_groups);
    for (std::size_t g = 0; g < n_groups; ++g)
      for (int seat = 0; seat < 2; ++seat) {
        sh[g][seat].assign(kRiverC, 0.0);
        for (std::uint32_t c = 0; c < kRiverC; ++c)
          sh[g][seat][c] = uval(arm, g, seat, c);
      }
    return sh;
  };

  if (arm_set != "spike" && arm_set != "prod" && arm_set != "prodfine" && arm_set != "full" &&
      arm_set != "corner") {
    for (const char* arm : {"zm", "coh", "pat", "act"}) {
      const Shifts sh = pattern_shifts(arm);
      for (double level : kLevels)
        inject(arm, level, sh);
    }
  }

  // Partially-coherent BR-weight-ranked spike arms at the operating amplitude
  // E (and a 1e-2 bracket point). q is the fraction of positive-weight rho mass
  // driven to a +E payoff shift; q ~= (L/E)^2 lands at e_l2 ~= L. q=1 is the
  // all-combo coherent cross-check. Labels carry q in the arm field.
  if (arm_set == "prod") {
    // Production-leaf box corner: candidate-only leaves (river_iters passed
    // on the CLI), fine concentrations at amplitude E only. q=.01/.02 bracket
    // the bulk of the production L*; prodfine (.002/.005) brackets the
    // smallest L* (raised .00043) so the box corner is interpolated/measured,
    // not extrapolated to q->0. Absolute delta rises monotonically with q on
    // every measured stratum, so finer points bound smaller-L* corners.
    constexpr double kE = 0.0184;
    for (double q : {0.01, 0.02}) {
      const Shifts sh = spike_shifts(q);
      char label[24];
      std::snprintf(label, sizeof(label), "spike%.3f", q);
      inject(label, kE, sh);
    }
  } else if (arm_set == "prodfine") {
    constexpr double kE = 0.0184;
    for (double q : {0.002, 0.005}) {
      const Shifts sh = spike_shifts(q);
      char label[24];
      std::snprintf(label, sizeof(label), "spike%.3f", q);
      inject(label, kE, sh);
    }
  } else if (arm_set == "full") {
    // Definitive single campaign: production candidate+reference (real
    // drift/L* via the norms sidecar), identity, and the four fine spikes at
    // E, each carrying its analytic first-order box bound column (so measured
    // delta vs the global-pooled analytic bound is cross-checked in one run).
    constexpr double kE = 0.0184;
    for (double q : {0.002, 0.005, 0.01, 0.02}) {
      const Shifts sh = spike_shifts(q);
      char label[24];
      std::snprintf(label, sizeof(label), "spike%.3f", q);
      inject(label, kE, sh);
    }
  } else if (arm_set == "corner") {
    // R1: inject the EXACT box-corner optimizer d* at the stratum's own
    // measured full-census envelope (e_max_real, L*), sign(w)-signed so both
    // w tails are exercised. level=1 because corner_shifts already holds the
    // dimensionless d*. The row's analytic_bound_bb must dominate the measured
    // delta, pinning the certificate at its claimed point rather than via a
    // q-family interpolation.
    const Shifts sh = corner_shifts(emax_real, lstar_real);
    inject("corner", 1.0, sh);
  } else if (arm_set != "patterns") {
    constexpr double kE = 0.0184;
    for (double q : {0.02, 0.033, 0.05}) {
      const Shifts sh = spike_shifts(q);
      char label[24];
      std::snprintf(label, sizeof(label), "spike%.3f", q);
      inject(label, 1e-2, sh);
      inject(label, kE, sh);
    }
    {
      const Shifts sh = spike_shifts(1.0);
      inject("spike1.0", kE, sh);
    }
  }

  if (csv != nullptr) {
    std::fclose(csv);
  }
  return 0;
}
