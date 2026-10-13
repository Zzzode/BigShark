// RFC 0013 Stage 2: NSeatPolicy binary serialization.
//
// Saves and loads the sealed policy rows from a postflop CFR solve so the
// on-the-fly evaluator can rebuild the tree and compute root values for
// deals not present in the declared frontier table. The GameDef and
// AbstractionId are NOT stored in the file — they are reconstructed from the
// (flop_class, spr_bucket) config by the caller, so the file contains only
// the (info-set key → action probabilities) rows.
//
// File layout (little-endian x86_64 Linux):
//   Header (36 bytes):
//     char     magic[4]        = "NSP1"
//     uint32   version         = 1
//     uint8    seat_count      (2)
//     uint8    spr_bucket      (0..5)
//     uint16   reserved        = 0
//     uint32   row_count
//     int32    flop_class[3]   (12 bytes)
//     uint8    padding[8]      (zero)
//   For each row:
//     uint32   own_card_bucket
//     uint64   node_index
//     uint32   action_count
//     uint8    padding[4]      (zero)
//     For each action:
//       uint8    type          (0=Fold, 1=Check, 2=Call, 3=Bet, 4=Raise)
//       uint8    padding[3]    (zero)
//       uint32   target_total
//     double   probabilities[action_count]
#pragma once

#include <bs/nseat_trainer.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace bs::solver {

// Save the policy rows to a binary file. The spr_bucket is stored in the
// header and verified on load. Writes are atomic (temp file + rename) so a
// crash mid-write does not leave a corrupt file. Throws std::runtime_error
// on I/O failure or std::invalid_argument on an unexpected action type.
void save_nseat_policy(const NSeatPolicy& policy, std::uint8_t spr_bucket,
                       const std::filesystem::path& path);

// Load policy rows from a binary file. The GameDef, AbstractionId, and
// expected spr_bucket are passed so the returned policy is sealed against
// the correct tree and the header is fully verified. Throws
// std::runtime_error on I/O failure, format mismatch, or corrupt data.
// max_bytes bounds simultaneous input buffers and conservative decoded-policy
// accounting; tree::tree_resource_exhausted is thrown before a crossing allocation.
NSeatPolicy load_nseat_policy(const std::filesystem::path& path, const poker::GameDef& game,
                              const abstraction::AbstractionId& action_id,
                              std::uint8_t expected_spr_bucket,
                              std::size_t max_bytes = std::size_t{1} << 30);

// Conservative retained bytes including the policy object, action-identity
// strings, map-node overhead, and action/probability vector capacities.
// Returns SIZE_MAX on arithmetic overflow, so cache admission fails closed.
std::size_t nseat_policy_accounted_bytes(const NSeatPolicy& policy) noexcept;

// Compute the policy file name for a (flop_class, spr_bucket) config.
std::string policy_file_name(std::span<const int> flop_class, std::uint8_t spr_bucket);

}  // namespace bs::solver
