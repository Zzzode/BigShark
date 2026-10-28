// RFC 0039: nested river continuation values — leaf identity and nested
// river-rooted game construction.
//
// The nested river subgame is the smallest subgame containing the betting
// the M2 measurement proved missing: exactly one betting street (river),
// built as a street-start river-rooted GameDef from the OUTER turn tree's
// frontier leaf ledger, with the outer ActionAbstraction inherited verbatim.
//
// Two identity traps are pinned here (independent review, RFC 0039):
//  - The button is the OUTER game's button. SubgameResolver's
//    build_subgame_def_from_node rotates button=(hero+n-1)%n to force the
//    hero first for a MID-STREET resolve; doing that here would permute the
//    river opener and every river menu, breaking correspondence with the
//    full turn->river reference subtree.
//  - Street state is the post-river-card street-start state: the builder's
//    after_card already reset street_committed to 0, raises_this_street to
//    0, and last_full_raise to the street-start sentinel before the frontier
//    leaf was emitted, and turn wagers are already inside the ledger's
//    (gross/refunded) contributions. Re-introducing any of them would
//    double-count the pot or suppress river raises under the cap.
#include <bs/continuation_value.hpp>
#include <stdexcept>

namespace bs::gto {

RiverBoundaryLeaf make_river_boundary_leaf(const tree::AbstractTree& outer_tree,
                                           std::size_t frontier_node_index) {
  const tree::TreeNode& node = outer_tree.node(frontier_node_index);
  if (node.kind != tree::NodeKind::TerminalFrontier)
    throw std::invalid_argument("make_river_boundary_leaf: node is not a TerminalFrontier leaf");
  if (node.terminal == tree::kNoNode)
    throw std::invalid_argument("make_river_boundary_leaf: frontier node has no payload");
  const tree::TerminalPayload& payload = outer_tree.terminal(node.terminal);
  if (payload.board_size != 5)
    throw std::invalid_argument(
        "make_river_boundary_leaf: nested continuation requires a five-card river board");

  RiverBoundaryLeaf leaf;
  leaf.outer_node_index = frontier_node_index;
  leaf.outer_action_id = outer_tree.action_id();
  leaf.outer_def = outer_tree.def();
  leaf.ledger = payload;
  return leaf;
}

// Build the street-start river-rooted game for one boundary leaf. Throws
// std::invalid_argument when the leaf cannot host a river betting subgame
// (fewer than two live seats); every other malformed shape is rejected by
// poker::validate inside the nested tree constructor.
poker::GameDef nested_river_game(const RiverBoundaryLeaf& leaf) {
  const tree::TerminalPayload& payload = leaf.ledger;
  if (payload.player_count < 2 || payload.live_count < 2)
    throw std::invalid_argument(
        "nested_river_game: a river betting subgame needs at least two live seats");

  poker::GameDef def{};
  def.player_count = payload.player_count;
  // Keep the OUTER button and posting identity.
  def.button = leaf.outer_def.button;
  def.big_blind = leaf.outer_def.big_blind;
  def.ante = leaf.outer_def.ante;
  def.variant = leaf.outer_def.variant;

  poker::Chips pot = 0;
  for (std::size_t s = 0; s < payload.player_count; ++s) {
    const tree::TerminalSeat& seat = payload.seats[s];
    def.stacks[s] = seat.stack;
    // NET contributions: an unmatched all-in refund is already separated in
    // the ledger. Folded seats keep their dead money in the pot.
    def.contributions[s] = seat.contributed - seat.refunded;
    def.folded[s] = seat.folded;
    pot += def.contributions[s];
    // Street-start defaults: street_committed/raise_rights/
    // last_full_raise/raises_this_street stay zero/false; the GameState
    // constructor derives raise_rights = !folded when every street
    // commitment is zero.
  }
  def.pot = pot;

  for (std::size_t i = 0; i < 5; ++i)
    def.board[i] = payload.board[i];
  def.board_size = 5;
  def.preflop = false;
  def.terminal = poker::TerminalDepth::River;
  return def;
}

}  // namespace bs::gto
