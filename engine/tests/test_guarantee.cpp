// RFC 0008 stage 5 guarantee-level tests: the five-level ladder, rank/order,
// the token vocabulary, and the SINGLE normative source->level table. Every
// policy-producible source is Approximate today; the abstract/exact levels
// exist but are not earned until a measured promotion (stage 6).
#include <array>
#include <bs/guarantee.hpp>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, const std::string& description) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", description.c_str());
    ++failures;
  }
}

constexpr bool everySourceMappedBelowExact() {
  bool allApproximate = true;
  bs::visitEverySource([&](bs::DecisionSource source) {
    const bs::Guarantee level = bs::guaranteeFor(source);
    allApproximate = allApproximate && level == bs::Guarantee::Approximate;
  });
  return allApproximate;
}

}  // namespace

int main() {
  using namespace bs;

  // --- Ladder order --------------------------------------------------------
  const std::array<Guarantee, 5> ascending = {Guarantee::OperationalFallback,
                                              Guarantee::Approximate, Guarantee::AbstractSolved,
                                              Guarantee::ExactSolved, Guarantee::CertifiedBound};
  for (std::size_t i = 0; i < ascending.size(); ++i) {
    check(guaranteeRank(ascending[i]) == static_cast<int>(i),
          "rank is the enumerator ordinal " + std::to_string(i));
    for (std::size_t j = 0; j < ascending.size(); ++j)
      check(guaranteeMeets(ascending[i], ascending[j]) == (i >= j),
            "guaranteeMeets is a total order");
  }
  check(guaranteeMeets(Guarantee::Approximate, Guarantee::OperationalFallback),
        "approximate meets operational floor");
  check(!guaranteeMeets(Guarantee::Approximate, Guarantee::AbstractSolved),
        "approximate does not meet abstract floor");
  check(guaranteeMeets(Guarantee::CertifiedBound, Guarantee::CertifiedBound),
        "a level meets itself");

  // --- Tokens: exactly the five wire vocabulary words, once each ----------
  const std::array<std::string_view, 5> tokens = {
      guaranteeToken(Guarantee::OperationalFallback), guaranteeToken(Guarantee::Approximate),
      guaranteeToken(Guarantee::AbstractSolved), guaranteeToken(Guarantee::ExactSolved),
      guaranteeToken(Guarantee::CertifiedBound)};
  const std::array<std::string_view, 5> expected = {
      "operational_fallback", "approximate", "abstract_solved", "exact_solved", "certified_bound"};
  for (std::size_t i = 0; i < tokens.size(); ++i)
    check(tokens[i] == expected[i], "canonical token " + std::string(expected[i]));
  for (std::size_t i = 0; i < tokens.size(); ++i)
    for (std::size_t j = 0; j < tokens.size(); ++j)
      check(i == j || tokens[i] != tokens[j], "guarantee tokens are unique");

  // --- The normative table -------------------------------------------------
  check(guaranteeFor(DecisionSource::PreflopChart) == Guarantee::Approximate,
        "preflop chart is approximate");
  check(guaranteeFor(DecisionSource::PostflopHeuristic) == Guarantee::Approximate,
        "postflop heuristic is approximate");
  check(guaranteeFor(DecisionSource::RiverLp) == Guarantee::Approximate,
        "river LP over capped ranges is approximate, not exact");
  check(guaranteeFor(DecisionSource::RiverDcfr) == Guarantee::Approximate,
        "bounded river DCFR is approximate");
  check(guaranteeFor(DecisionSource::MultistreetCfr) == Guarantee::Approximate,
        "offline multistreet CFR is approximate");

  // No source today may earn abstract_solved or exact_solved. Promoting one
  // requires measured bounds and must change this test deliberately.
  check(everySourceMappedBelowExact(),
        "no policy source earns abstract_solved/exact_solved before stage 6");

  // Compile-time visitor covers every enumerator (a sixth source that
  // guaranteeFor misses still instantiates here; the -Werror=switch switch
  // itself fails the build first).
  int visited = 0;
  visitEverySource([&](DecisionSource) { ++visited; });
  check(visited == 5, "visitor enumerates all five policy sources");

  if (failures != 0) {
    std::fprintf(stderr, "GUARANTEE TESTS FAILED: %d\n", failures);
    return 1;
  }
  std::puts("GUARANTEE TESTS PASSED");
  return 0;
}
