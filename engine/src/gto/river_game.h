// River poker game for OpenSpiel infostate-tree sequence-form Nash solving.
// See river_game.cc for the public-tree / payoff description.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "bs/range.hpp"
#include "open_spiel/spiel.h"

namespace bigshark {

class RiverState;
class RiverGame;

// One weighted private deal: the chance action that reaches it from the root,
// the two combo indices, and its joint reach probability (normalized over all
// valid, non-blocking pairs whose combos carry positive range weight).
struct RiverDeal {
  open_spiel::Action chance_action;
  int ip_combo, oop_combo;
  double prob;
};

// Enumerate every legal (IP,OOP) combo pair weighted by wIP[i]*wOOP[j],
// normalized to sum 1. Uniform ranges reproduce the game's own chance node.
std::vector<RiverDeal> EnumerateRiverDeals(const RiverGame& game, const bs::Range& w_ip,
                                           const bs::Range& w_oop);

// All legal (IP,OOP) combo pairs in chance-action order. Shared between the
// game and its states so a chance action decodes to its pair in O(1) rather
// than rescanning the full 1326x1326 grid on every deal.
using RiverDealTable = std::vector<std::pair<int, int>>;
std::shared_ptr<const RiverDealTable> MakeRiverDealTable(const std::vector<int>& board);

class RiverGame : public open_spiel::Game {
 public:
  RiverGame(std::vector<int> board, int pot, float betFrac, float raiseFrac);

  std::unique_ptr<open_spiel::State> NewInitialState() const override;
  int NumDistinctActions() const override { return 3; }
  int NumPlayers() const override { return 2; }
  double MinUtility() const override;
  double MaxUtility() const override;
  int MaxGameLength() const override { return 4; }

  const std::vector<int>& Board() const { return board_; }
  int Pot() const { return pot_; }
  float BetFrac() const { return betFrac_; }
  float RaiseFrac() const { return raiseFrac_; }
  const std::shared_ptr<const RiverDealTable>& Deals() const { return deals_; }

 private:
  std::vector<int> board_;
  int pot_;
  float betFrac_, raiseFrac_;
  std::shared_ptr<const RiverDealTable> deals_;
};

class RiverState : public open_spiel::State {
 public:
  RiverState(std::shared_ptr<const open_spiel::Game> game, const std::vector<int>& board, int pot,
             float betFrac, float raiseFrac, std::shared_ptr<const RiverDealTable> deals);
  RiverState(const RiverState&) = default;

  open_spiel::Player CurrentPlayer() const override;
  std::string ActionToString(open_spiel::Player p, open_spiel::Action a) const override;
  std::string ToString() const override;
  bool IsTerminal() const override;
  std::vector<double> Returns() const override;
  std::string InformationStateString(open_spiel::Player player) const override;
  std::string ObservationString(open_spiel::Player player) const override;
  void InformationStateTensor(open_spiel::Player, absl::Span<float>) const override {}
  void ObservationTensor(open_spiel::Player, absl::Span<float>) const override {}
  std::unique_ptr<open_spiel::State> Clone() const override;
  std::vector<std::pair<open_spiel::Action, double>> ChanceOutcomes() const override;
  std::vector<open_spiel::Action> LegalActions() const override;

 protected:
  void DoApplyAction(open_spiel::Action a) override;

 private:
  int Winner() const;  // 0=IP 1=OOP 2=chop
  double PayoffIP() const;

  std::vector<int> board_;
  int pot_;
  float betFrac_, raiseFrac_;
  std::shared_ptr<const RiverDealTable> deals_;
  bool cardsDealt_ = false;
  int ipCombo_ = -1, oopCombo_ = -1;
  std::string h_;
};

// Register-free factory (no spiel registry / spiel.cc dependency).
std::shared_ptr<const RiverGame> MakeRiverGame(const std::vector<int>& board, int pot,
                                               float betFrac, float raiseFrac);

}  // namespace bigshark
