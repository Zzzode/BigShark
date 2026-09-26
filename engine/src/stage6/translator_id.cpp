#include <bs/stage6/translator_id.hpp>

namespace bs::stage6 {

namespace {

// 64-bit FNV-1a over the rule parameter text, the same hash family the rest of
// the stage-6 core uses for content-derived digests. The translator digest is
// pure identity (the projection rule text), so it belongs in core and eval
// merely returns it.
std::uint64_t fnv1a(const std::string& bytes) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (unsigned char byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

constexpr const char* kRuleParameters =
    "nearest-legal-integer;ties=smaller;fallback=call-check-fold";

}  // namespace

TranslatorId nearest_target_translator_id() {
  TranslatorId id;
  id.name = "rfc0008-coarse-to-exact";
  id.version = 1;
  id.parameters = kRuleParameters;
  id.digest = fnv1a(id.name + ":v" + std::to_string(id.version) + ":" + id.parameters);
  return id;
}

}  // namespace bs::stage6
