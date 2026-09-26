// stage6/translator_id.hpp — versioned identity of the coarse<->exact
// translator, shared by the trainer's frozen manifest (core, no policy deps)
// and the translator implementation (eval).
#pragma once

#include <cstdint>
#include <string>

namespace bs::stage6 {

struct TranslatorId {
  std::string name = "rfc0008-coarse-to-exact";
  std::uint32_t version = 1;
  // Declared projection rule parameters (round-half tie direction, fallback
  // chain). Empty for the frozen v1 rule.
  std::string parameters;
  std::uint64_t digest = 0;

  std::string to_string() const { return name + ":v" + std::to_string(version); }
  bool operator==(const TranslatorId&) const = default;
};

// The frozen v1 translator identity. The digest is computed by the eval-side
// translator component (which owns the rule text); core treats it as an opaque
// recorded value, so this returns name/version/parameters with digest 0 and
// the eval side fills the digest when a manifest is finalized.
TranslatorId nearest_target_translator_id();

}  // namespace bs::stage6
