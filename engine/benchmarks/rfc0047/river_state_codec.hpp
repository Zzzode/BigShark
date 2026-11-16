// RFC 0050 D307 bench-only resumable river state codec.
//
// A long-lived binary checkpoint for the seedless exact-menu river kernel.
// The format is versioned and self-describing: a fixed-width header carries
// the magic, format version, full GameDef identity, the action-abstraction
// digest, structural node counts, the frozen Stage 1 constants (nine-rung
// ladder and 0.002 BB gate), solver-mode flags, payload length, and an
// FNV-1a 64-bit checksum covering the header prefix and every payload byte.
// Loading validates every field and rebuilds the cap-0 tree to cross-check
// per-node widths; any mismatch throws rather than attempting a best-effort
// recovery. This component is benchmark-only under the rfc0047 link guard
// and defines no production or persisted product format.
#pragma once

#include <cstdint>
#include <filesystem>

#include "river_solver.hpp"

namespace bs::rfc0047 {

inline constexpr std::uint32_t kRiverCheckpointFormatVersion = 1;
inline constexpr std::uint64_t kRiverCheckpointMagic = 0x5246433035304350ULL;  // "RFC050CP"

// Serialize the resumable state with the solver-mode identity and the
// compile-time frozen constants baked into the header. Throws on I/O
// failure; never partially writes a valid-looking file (writes a temp
// sibling and renames into place).
void save_river_checkpoint(const std::filesystem::path& path, const RiverResumableState& state,
                           bool rm_plus, bool linear_averaging, bool dcfr);

// Read and validate a checkpoint against the expected game and action
// digest, the embedded frozen constants, the rebuilt cap-0 tree shape, and
// the checksum. Returns the imported resumable state only when every check
// passes; throws std::runtime_error with a specific reason otherwise.
RiverResumableState load_river_checkpoint(const std::filesystem::path& path,
                                          const bs::poker::GameDef& expected_def,
                                          std::uint64_t expected_action_digest);

// Number of payload bytes (per-node slot counts plus the regret/sum
// doubles) for a state, so a probe can report the write cost without
// touching disk.
std::uint64_t river_checkpoint_payload_bytes(const RiverResumableState& state);

// Serialize the full checkpoint (header, checksum, payload) to an in-memory
// buffer without any file I/O, so a probe can separate the unavoidable
// memory copy cost from disk write cost.
std::string serialize_river_checkpoint(const RiverResumableState& state, bool rm_plus,
                                       bool linear_averaging, bool dcfr);

}  // namespace bs::rfc0047
