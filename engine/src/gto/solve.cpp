#include <bs/detail/solve_projection.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/solve.hpp>
#include <sstream>

namespace bs::solver {

namespace {

using poker::GameDef;

// The seat-parameterized MCCFR trainer (RFC 0009 D2/W2a), routed for identity
// trees at 3+ seats under Auto and explicitly selectable at any seat count.
// Everything this pre-validates has a typed solver-selection refusal so the
// trainer's invalid_argument family is reached only for genuinely malformed
// ranges (which the host should treat as a bad request); the training itself
// runs under the request's limits (tree build + sampled walk).
SolveResult solve_nseat(const SolveRequest& request) {
  const tree::AbstractTree& tree = *request.tree;
  const GameDef& def = tree.def();

  if (def.player_count < 2 || def.player_count > poker::kMaxUnifiedSeats)
    throw unsupported_tree_shape("n-seat CFR supports 2..10 seat trees");
  // The n-seat trainer accepts the identity and any declared-coarse action
  // abstraction (RFC 0008 DeclaredOnly menu); validate_request enforces this.
  if (tree.action_id() != abstraction::identity_action_id() &&
      !abstraction::is_declared_coarse_action_id(tree.action_id()))
    throw unsupported_tree_shape(
        "n-seat CFR requires the identity or a declared-coarse action abstraction");
  if (request.ranges.size() != def.player_count)
    throw unsupported_tree_shape("n-seat CFR needs one range per game seat");
  if (request.iterations == 0)
    throw std::invalid_argument("solve requires a positive iteration count");
  // The unabstracted legal game is unbounded, so every tree root this route
  // accepts is a postflop root (the card abstraction is defined on a complete
  // flop); a preflop tree also cannot materialize under any sane node cap.
  // RFC 0007 flop-terminal preflop roots are trained through train_nseat
  // directly with a FrontierEvaluator, not through this solve() route.
  if (def.board_size < 3 || def.board_size > 5)
    throw unsupported_tree_shape(
        "n-seat CFR requires a postflop root; flop-terminal preflop games use "
        "train_nseat with a frontier evaluator");
  // Fixed runout conditioning is a stage-4 heads-up feature. Forwarding it
  // would silently DROP the conditioning (the n-seat trainer samples chance
  // unconditionally), so refuse with the typed shape error instead.
  if (request.runout.fixed_turn.has_value() || request.runout.fixed_river.has_value())
    throw unsupported_tree_shape("fixed runout conditioning is not supported by n-seat CFR");
  // The n-seat walk consumes no SolveMode: it is an external-sampling trainer
  // by definition. An explicit FullTraversal request would be a mode the
  // routed model does not have, so refuse rather than substitute silently.
  if (request.mode == SolveMode::FullTraversal)
    throw unsupported_tree_shape(
        "n-seat CFR is an external-sampling trainer only; request ExternalSampling");

  NSeatTrainingResult result = train_nseat(tree, request.ranges, request.iterations, request.seed,
                                           request.limits.nseat_limits());
  return SolveResult(std::move(result), tree.action_id());
}

// The heads-up multistreet CFR, routed behind solve() with the numeric core
// unchanged. Validates the request describes a two-seat game under the
// identity action abstraction, projects to HeadsUpGame, and dispatches.
SolveResult solve_heads_up(const SolveRequest& request) {
  const tree::AbstractTree& tree = *request.tree;
  const GameDef& def = tree.def();

  if (def.player_count != 2)
    throw unsupported_tree_shape("heads-up CFR requires a two-seat tree");
  // HeadsUpRoot is structurally flop-rooted (its board slot is exactly three
  // cards); a turn- or river-rooted two-seat game cannot be projected without
  // silently dropping board[3..4]. Refuse the shape loudly; the n-seat route
  // solves such roots directly.
  if (!def.preflop && def.board_size != 3)
    throw unsupported_tree_shape(
        "heads-up CFR requires a flop-rooted tree (board_size 3); use n-seat CFR for a "
        "turn- or river-rooted two-seat game");
  // Identity action schedule by AbstractionId, compared to the schedule the
  // tree was built from (never by re-deriving menus from a second default).
  // Declared-coarse abstractions (RFC 0008 DeclaredOnly) are also accepted:
  // the heads-up trainer builds its menus from the schedule, and the tree's
  // abstract actions are already baked in by the AbstractTree construction.
  if (tree.action_id() != abstraction::identity_action_id() &&
      !abstraction::is_declared_coarse_action_id(tree.action_id()))
    throw unsupported_tree_shape(
        "heads-up CFR requires the identity or a declared-coarse action abstraction");
  if (request.ranges.size() != 2)
    throw unsupported_tree_shape("heads-up CFR needs exactly two seat ranges");
  if (request.iterations == 0)
    throw std::invalid_argument("solve requires a positive iteration count");

  // Fixed runout conditioning is a postflop feature: the trainer's runout slots
  // are turn/river only for a flop-rooted game. A preflop root fills the flop
  // one card at a time against the same slot, which cannot be pinned, so refuse
  // with the typed shape error instead of forwarding into an internal throw.
  if (def.preflop &&
      (request.runout.fixed_turn.has_value() || request.runout.fixed_river.has_value()))
    throw unsupported_tree_shape(
        "fixed runout conditioning is only supported on a flop-rooted tree");

  // Validate fixed conditioning HERE with the typed shape refusal: legal ids,
  // mutually distinct, off the rooted board, and off every dealt hole card.
  // The trainer would otherwise reject these via require() -> invalid_argument,
  // the wrong family for an expected solver-selection refusal. Presence is
  // tested with has_value() (not a negative sentinel), so a present -1 is an
  // out-of-range shape error rather than being mistaken for "no conditioning".
  const std::optional<int> fixed[2] = {request.runout.fixed_turn, request.runout.fixed_river};
  for (int i = 0; i < 2; ++i) {
    if (!fixed[i].has_value())
      continue;
    const int card = *fixed[i];
    if (card < 0 || card > 51)
      throw unsupported_tree_shape("fixed runout card id out of range");
    for (std::size_t b = 0; b < def.board_size; ++b)
      if (card == def.board[b])
        throw unsupported_tree_shape("fixed runout card is already on the board");
    if (i == 1 && fixed[0].has_value() && card == *fixed[0])
      throw unsupported_tree_shape("fixed turn and river cards must differ");
    for (const SeatRanges& seat : request.ranges)
      for (const WeightedHand& hand : seat)
        if (card == hand.cards[0] || card == hand.cards[1])
          throw unsupported_tree_shape("fixed runout card collides with a dealt hole card");
  }

  HeadsUpGame game;
  game.root = detail::project_heads_up_root(def);
  for (std::size_t seat = 0; seat < 2; ++seat)
    game.ranges[seat] = request.ranges[seat];
  game.sizes = tree.action_abstraction().schedule();
  game.fixed_runout = {request.runout.fixed_turn, request.runout.fixed_river};

  HeadsUpTrainer trainer(std::move(game));
  TrainingResult result = request.mode == SolveMode::ExternalSampling
                              ? trainer.train_sampled(request.iterations, request.seed,
                                                      request.limits.training_limits())
                              : trainer.train(request.iterations, request.limits.training_limits());
  return SolveResult(std::move(result), tree.action_id());
}

}  // namespace

// River LP/DCFR and the experimental multistreet trainer solve bespoke games
// (a six-node float-geometry river toy; an OCHS-bucketed experimental DCFR) that
// are NOT L1 GameDef games and have no identity L3 tree (RFC 0008 stage 4).
// They are reachable through solve() and explicitly refuse every L3 request;
// their numerics remain gated on their unchanged direct entry points. Their
// numeric ports are later stages that declare a dedicated abstraction.
[[noreturn]] static void refuse_non_l1(const char* which) {
  std::ostringstream msg;
  msg << which << " does not solve an L1 identity abstract tree (stage 4 refuse-only)";
  throw unsupported_tree_shape(msg.str());
}

SolveResult solve(const SolveRequest& request) {
  if (request.tree == nullptr)
    throw std::invalid_argument("solve request has no tree");
  const poker::GameDef& def = request.tree->def();

  // The non-L1 solvers are reachable by explicit selection and refuse every L3
  // tree this stage: the river LP/DCFR solve a six-node float-geometry toy that
  // is not an L1 GameDef, and the experimental multistreet trainer is an
  // OCHS-bucketed offline model. Their numerics remain gated on their direct
  // entry points; an honest typed refusal beats a fake numeric port.
  switch (request.solver) {
    case SolverKind::RiverLp:
      refuse_non_l1("river LP");
    case SolverKind::RiverDcfr:
      refuse_non_l1("river DCFR");
    case SolverKind::MultistreetCfr:
      refuse_non_l1("experimental multistreet CFR");
    case SolverKind::HeadsUpCfr:
      if (def.player_count != 2)
        throw unsupported_tree_shape("heads-up CFR requires a two-seat tree");
      return solve_heads_up(request);
    case SolverKind::NSeatCfr:
      // Explicitly selectable at any seat count, including two (conformance
      // fixtures compare the two routes on one game).
      return solve_nseat(request);
    case SolverKind::Auto:
      break;
  }

  if (def.variant != poker::RulesVariant::NoLimitHoldem)
    refuse_non_l1("unsupported rules variant");

  // Auto: the two-seat identity tree keeps routing to the heads-up multistreet
  // CFR bit-for-bit; 3..10-seat identity trees route to the seat-parameterized
  // MCCFR trainer, which refuses (with the same typed family) any shape it
  // does not model.
  if (def.player_count == 2)
    return solve_heads_up(request);

  return solve_nseat(request);
}

}  // namespace bs::solver
