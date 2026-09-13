// multistreet_cfr.h — multi-street (flop->turn->river) DCFR with public-card
// CHANCE nodes. Experimental, self-contained (no OpenSpiel/LP); the purpose is
// to learn equilibrium per-street continuation frequencies and equilibrium
// river ranges, eventually replacing the heuristic range tracker.
//
// Minimal betting tree per street (no raises for the validation build):
//   IP first:  check / bet
//   check -> OOP: check (next street) / bet ; if OOP bets, IP: fold / call
//   bet   -> OOP: fold / call
// A check-check or a call closes the street; on flop/turn a public card is
// dealt (chance), on the river it is a showdown. Strictly zero-sum, fixed
// chip commit per bet (b = bet_frac * starting pot), antes a = pot/2.
#pragma once
#include <array>
#include <bs/equity.hpp>
#include <bs/range.hpp>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace bs::gto {

struct MultiStreetOptions {
  int iterations = 50000;
  int max_combos_per_side = 6;  // validation build keeps ranges tiny
  float bet_frac = 0.75f;
  uint64_t seed = 1;
  int fixed_turn = -1;   // >=0: collapse chance to this turn card (testing)
  int fixed_river = -1;  // >=0: collapse chance to this river card (testing)
  int buckets = 12;      // OCHS hand-strength buckets shared across runouts
  int bucket_mc = 220;   // MC samples per bucket-equity evaluation
};

struct MultiStreetResult {
  bool ok = false;
  double value_to_ip = 0.0;
  double exploitability_pot = 1.0;
  int iterations_done = 0;
};

class MultiStreetSolver {
 public:
  MultiStreetSolver(std::vector<int> flop_board, int pot, const Range& ip_range,
                    const Range& oop_range, const MultiStreetOptions& opt);
  MultiStreetResult Solve();

  // Exact distribution-aggregated values for external validation.
  double BrValue(int resp, int own) const;
  double EqValue(int resp, int own) const;
  // external verification hooks
  int NumCombos(int player) const { return player == 0 ? (int)ipC_.size() : (int)oopC_.size(); }
  double ComboWeight(int player, int k) const { return player == 0 ? ipW_[k] : oopW_[k]; }
  int ComboId(int player, int k) const { return player == 0 ? ipC_[k] : oopC_[k]; }
  std::size_t InformationSetCount() const { return N_[0].size() + N_[1].size(); }
  bool Share(int ipIdx, int opIdx) const;
  float Bet() const { return bfrac_ * pot_; }
  int Pot() const { return pot_; }
  const std::vector<int>& Flop() const { return flop_; }
  // trained behavior prob for player/combo at a decision node on a dealt board
  double PolicyAt(int player, int combo, int street, int turn, int river, int hcode, int act,
                  int history = 0) const;

 private:
  struct ISet {
    int na = 2;
    std::array<double, 3> r{}, s{}, cur{0.5, 0.5, 0.0};
    void policy(double* o) const;
    void freeze();
    double avg(int a) const;
  };
  // info-sets are keyed by a hand-STRENGTH BUCKET (OCHS) instead of exact
  // turn/river, so learning is shared across all runouts where the combo lands
  // in the same strength class.
  uint64_t key(int combo, int street, int hcode, int bucket, int history) const;
  ISet& iset(int player, int combo, int street, int hcode, int bucket, int history);
  int bucketOf(int combo, int player, int turn, int river) const;
  int bucketCompute(int combo, int player, int turn, int river) const;
  int owner(int street, int hcode) const;
  double cfr(int i, int j, int turn, int river, int street, int hcode, int commits, double p0,
             double p1, int tr, int history);
  int drawCard(const bool used[52], bs::XorShift64& rng) const;
  bool share(int i, int j) const;
  void iterate();
  void discount(long long t);
  double exploitability();

  // Best-response and equilibrium values keep the opponent's hidden range as
  // a reach vector. A responder action is selected only after all compatible
  // opponent histories in the information set have been aggregated.
  double responseValue(int resp, int own, const std::vector<double>& opponent_reach, int turn,
                       int river, int street, int hcode, int commits, int history, bool best) const;
  std::vector<double> initialOpponentReach(int resp, int own) const;
  double polAvg(int player, int combo, int street, int hcode, int bucket, int act,
                int history) const;

  std::vector<int> flop_;
  int pot_;
  float bfrac_;
  MultiStreetOptions opt_;
  std::vector<int> ipC_, oopC_;
  std::vector<double> ipW_, oopW_;
  int Ni_ = 0, No_ = 0;
  double valueToIp_ = 0, gapI_ = 0, gapO_ = 0;
  std::unordered_map<uint64_t, ISet> N_[2];
  bs::XorShift64 rng_{1};
  int sampledTurn_ = -1, sampledRiver_ = -1;
  mutable std::unordered_map<uint64_t, int> bucketCache_;  // memoized bucket per (combo,board)
};

}  // namespace bs::gto
