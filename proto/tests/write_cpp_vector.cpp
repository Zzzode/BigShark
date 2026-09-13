#include <bigshark/engine/v1/engine.pb.h>

#include <filesystem>
#include <fstream>
#include <ios>

int main(int argc, char** argv) {
  if (argc != 2)
    return 2;

  bigshark::engine::v1::Envelope envelope;
  envelope.set_protocol_minor(1);
  envelope.set_request_id("cpp-vector");
  auto* capabilities = envelope.mutable_get_capabilities_response();
  capabilities->add_supported_protocol_minors(0);
  capabilities->set_engine_build_version("cpp-stage4-vector");
  capabilities->add_supported_game_variants(bigshark::engine::v1::GAME_VARIANT_NLHE);
  capabilities->add_supported_betting_structures(bigshark::engine::v1::BETTING_STRUCTURE_NO_LIMIT);
  capabilities->set_minimum_players(2);
  capabilities->set_maximum_players(6);
  capabilities->add_supported_streets(bigshark::engine::v1::STREET_PREFLOP);
  capabilities->add_supported_actions(bigshark::engine::v1::ACTION_TYPE_FOLD);
  capabilities->set_amount_semantics(bigshark::engine::v1::AMOUNT_SEMANTICS_TARGET_TOTAL_INCLUSIVE);
  capabilities->add_solver_modes(bigshark::engine::v1::SOLVER_MODE_AUTOMATIC);
  capabilities->add_strategy_profiles("tag");
  capabilities->set_exact_lp(bigshark::engine::v1::FEATURE_SUPPORT_SUPPORTED);
  capabilities->set_dcfr(bigshark::engine::v1::FEATURE_SUPPORT_SUPPORTED);
  capabilities->set_multistreet(bigshark::engine::v1::FEATURE_SUPPORT_EXPERIMENTAL);
  capabilities->set_side_pots(bigshark::engine::v1::FEATURE_SUPPORT_UNSUPPORTED);
  capabilities->set_rake(bigshark::engine::v1::FEATURE_SUPPORT_UNSUPPORTED);
  capabilities->set_tournament_icm(bigshark::engine::v1::FEATURE_SUPPORT_UNSUPPORTED);
  capabilities->set_maximum_request_bytes(1'048'576);
  capabilities->set_maximum_solve_time_ms(120'000);

  const std::filesystem::path output_path(argv[1]);
  std::filesystem::create_directories(output_path.parent_path());
  std::ofstream output(output_path, std::ios::binary);
  return envelope.SerializeToOstream(&output) ? 0 : 1;
}
