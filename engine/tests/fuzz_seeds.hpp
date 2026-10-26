// fuzz_seeds.hpp — structured current-schema seeds for protocol fuzzing.
//
// RFC 0034: there is one unversioned wire form, so the seed set uses current
// messages instead of a generation ladder. Legal seeds are shared with the
// conformance suite (conformance_fixtures.hpp); this file adds the open-enum
// attacks, oversized integers, capability and response frames, and a wrong
// protocol_version. Seeds are split at construction into a legal set (which
// the campaign mutates) and a negative set (which the oracle requires to
// never produce a strategy decision).
#pragma once

#include <bigshark/engine/engine.pb.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "conformance_fixtures.hpp"

namespace bs::fuzz {

namespace pe = ::bigshark::engine;
namespace cf = bs::conformance;

struct SeedSets {
  std::vector<std::string> legal;
  std::vector<std::string> negative;
};

// Serialize an envelope as a plain payload; the caller may frame it.
inline std::string frame(const pe::Envelope& envelope) {
  return envelope.SerializeAsString();
}

inline pe::Envelope envelopeForVersion(pe::DecisionRequest request, const char* id,
                                       std::uint32_t version) {
  pe::Envelope envelope;
  envelope.set_protocol_version(version);
  envelope.set_request_id(id);
  *envelope.mutable_decision_request() = std::move(request);
  return envelope;
}

inline void addNegative(SeedSets& sets, pe::Envelope envelope) {
  sets.negative.push_back(frame(envelope));
}

inline void addNegativeRequest(SeedSets& sets, pe::DecisionRequest request, const char* id) {
  addNegative(sets, envelopeForVersion(std::move(request), id, 3));
}

inline SeedSets buildSeedSets() {
  SeedSets sets;

  // --- Legal seeds the campaign mutates ------------------------------------
  sets.legal.push_back(cf::capabilitiesEnvelope("capabilities"));
  {
    pe::Envelope response;
    response.set_protocol_version(3);
    response.set_request_id("capabilities-response");
    response.mutable_get_capabilities_response()->add_capabilities();
    sets.legal.push_back(frame(response));
  }
  // The conformance vector 1 line, HEURISTIC mode so it reaches the decision
  // arm without resident services.
  sets.legal.push_back(frame(envelopeForVersion(cf::vector1Request(), "valid", 3)));

  // --- Negative: open-enum attacks on every validated enum -----------------
  {
    pe::DecisionRequest request = cf::vector1Request();
    request.mutable_state()->mutable_legal_actions(1)->set_type(static_cast<pe::ActionType>(99));
    addNegativeRequest(sets, std::move(request), "type99");
  }
  {
    pe::DecisionRequest request = cf::vector1Request();
    request.mutable_state()->mutable_legal_actions(1)->set_type(
        static_cast<pe::ActionType>(static_cast<std::uint32_t>(-1)));
    addNegativeRequest(sets, std::move(request), "typeneg");
  }
  {
    pe::DecisionRequest request = cf::vector1Request();
    request.mutable_state()->set_street(static_cast<pe::Street>(99));
    addNegativeRequest(sets, std::move(request), "street99");
  }
  {
    pe::DecisionRequest request = cf::vector1Request();
    request.mutable_state()->mutable_hero_hole_cards(0)->set_rank(static_cast<pe::Rank>(99));
    addNegativeRequest(sets, std::move(request), "rank99");
  }
  {
    pe::DecisionRequest request = cf::vector1Request();
    request.mutable_state()->mutable_board(0)->set_suit(static_cast<pe::Suit>(99));
    addNegativeRequest(sets, std::move(request), "suit99");
  }
  {
    pe::DecisionRequest request = cf::vector1Request();
    request.mutable_options()->set_solver_mode(static_cast<pe::SolverMode>(99));
    addNegativeRequest(sets, std::move(request), "solver99");
  }
  {
    pe::DecisionRequest request = cf::vector1Request();
    request.mutable_state()->mutable_players(1)->set_status(static_cast<pe::PlayerStatus>(99));
    addNegativeRequest(sets, std::move(request), "status99");
  }
  {
    pe::DecisionRequest request = cf::vector1Request();
    request.mutable_options()->set_utility_profile(static_cast<pe::UtilityProfile>(99));
    addNegativeRequest(sets, std::move(request), "utility99");
  }
  {
    pe::DecisionRequest request = cf::vector1Request();
    request.mutable_state()->mutable_game()->set_variant(static_cast<pe::GameVariant>(99));
    addNegativeRequest(sets, std::move(request), "variant99");
  }
  {
    pe::DecisionRequest request = cf::vector1Request();
    request.mutable_state()->mutable_game()->set_betting_structure(
        static_cast<pe::BettingStructure>(99));
    addNegativeRequest(sets, std::move(request), "structure99");
  }
  {
    pe::DecisionRequest request = cf::vector1Request();
    request.mutable_state()->mutable_game()->set_game_type(static_cast<pe::GameType>(99));
    addNegativeRequest(sets, std::move(request), "gametype99");
  }
  {
    pe::DecisionRequest request = cf::vector1Request();
    request.mutable_options()->set_minimum_guarantee(static_cast<pe::GuaranteeLevel>(99));
    addNegativeRequest(sets, std::move(request), "guarantee99");
  }

  // --- Negative: oversized chip values -------------------------------------
  {
    pe::DecisionRequest request = cf::vector1Request();
    request.mutable_state()->mutable_pot()->set_pot_total(1ull << 40);
    addNegativeRequest(sets, std::move(request), "hugepot");
  }
  {
    pe::DecisionRequest request = cf::vector1Request();
    constexpr std::uint64_t two_billion = 2'000'000'000ull;
    request.mutable_state()->mutable_pot()->set_pot_total(two_billion);
    request.mutable_state()->set_to_call(two_billion);
    addNegativeRequest(sets, std::move(request), "intoverflow");
  }
  {
    pe::DecisionRequest request = cf::vector1Request();
    constexpr std::uint64_t int_max = 2'147'483'647ull;
    request.mutable_state()->mutable_pot()->set_pot_total(int_max);
    request.mutable_state()->set_to_call(1);
    addNegativeRequest(sets, std::move(request), "intmaxedge");
  }

  // --- Negative: a decision RESPONSE frame the host must reject ------------
  {
    pe::Envelope response;
    response.set_protocol_version(3);
    response.set_request_id("decision-response-seed");
    pe::Strategy* strategy = response.mutable_decision_response()->mutable_strategy();
    strategy->mutable_selected_action()->set_type(pe::ACTION_TYPE_CHECK);
    addNegative(sets, std::move(response));
  }

  // --- Negative: wrong transport versions (the accepted value is exactly 3)
  for (std::uint32_t version : {0u, 1u, 2u, 4u, 0xffffffffu}) {
    addNegative(sets, envelopeForVersion(cf::vector1Request(), "badversion", version));
  }
  return sets;
}

// All structured seeds (legal and negative); the standalone driver and the
// libFuzzer entry point mutate across both.
inline std::vector<std::string> structuredFuzzSeeds() {
  const SeedSets sets = buildSeedSets();
  std::vector<std::string> all = sets.legal;
  all.insert(all.end(), sets.negative.begin(), sets.negative.end());
  return all;
}

// The structured negative set for the harness oracle.
inline std::vector<std::string> negativeFuzzSeeds() {
  return buildSeedSets().negative;
}

}  // namespace bs::fuzz
