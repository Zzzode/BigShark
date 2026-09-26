// stage6/frozen_manifest.hpp — frozen identity and on-disk artifact rows for a
// trained postflop geometry bucket.
//
// This is in bigshark_stage6_core (poker + abstraction only, no policy): the
// trainer writes artifacts and the evaluation candidate reads them, so the
// canonical blob format and content hashing must be shared but must never pull
// the deployed policy into the trainer. FNV-1a is the in-core rows hash; the
// outer lock SHA-256 is computed by the measurement driver (which links
// OpenSSL through eval).
#pragma once

#include <bs/abstraction.hpp>
#include <bs/stage6/geometry.hpp>
#include <bs/stage6/infoset_id.hpp>
#include <bs/stage6/translator_id.hpp>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace bs::poker {
struct Action;
}

namespace bs::stage6 {

class frozen_artifact_error : public std::runtime_error {
 public:
  explicit frozen_artifact_error(const std::string& what) : std::runtime_error(what) {}
};

// One frozen average-policy row over the representative's coarse menu.
struct AbstractPolicyRow {
  // Concrete representative-chip menu actions (L2 abstract menu), in order.
  std::vector<poker::Action> abstract_actions;
  // Average strategy probabilities; same length, sums to 1.
  std::vector<double> probabilities;
  std::uint64_t visits = 0;

  bool operator==(const AbstractPolicyRow&) const = default;
};

struct FrozenManifest {
  bs::abstraction::AbstractionId action_abstraction;
  bs::abstraction::AbstractionId card_abstraction;
  TranslatorId translator;
  std::uint64_t geometry_matrix_hash = 0;
  GeometryBucketKey bucket;
  std::string chart_digest_sha256;
  std::uint64_t training_config_hash = 0;
  // Iterations ACTUALLY completed before sealing (a wall-capped run seals
  // fewer than the requested count). Persisted so a loader can never mistake
  // zero/incomplete work for the requested iteration count.
  std::uint64_t iterations_completed = 0;
  // The artifact-identity stamp carried by EVERY sealed AbstractInfosetKey in
  // rows. A reader addresses a row with this exact stamp.
  std::uint64_t artifact_content_hash = 0;
  // FNV-1a over the canonical row bytes; the single content check the loader
  // recomputes. Equality is structural, not digest-based.
  std::uint64_t rows_content_hash = 0;
  std::uint64_t iterations = 0;
  std::uint64_t master_seed = 0;

  bool operator==(const FrozenManifest&) const = default;
};

using FrozenArtifactRows = std::map<AbstractInfosetKey, AbstractPolicyRow>;

// FNV-1a content hash of a row set in canonical key order.
std::uint64_t hash_artifact_rows(const FrozenArtifactRows& rows);

// Writes one bucket artifact directory: rows.bin (little-endian canonical
// blob) + manifest.bin. Deterministic for (manifest, rows).
void write_artifact_dir(const std::string& dir, const FrozenManifest& manifest,
                        const FrozenArtifactRows& rows);

// In-memory loaded artifact: the rows plus the verified manifest. Loading
// recomputes rows_content_hash and every identity field, throwing
// frozen_artifact_error on drift.
struct LoadedFrozenArtifact {
  FrozenManifest manifest;
  FrozenArtifactRows rows;
};
LoadedFrozenArtifact load_artifact_dir(const std::string& dir);

}  // namespace bs::stage6
