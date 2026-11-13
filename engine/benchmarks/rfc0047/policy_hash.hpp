// RFC 0048 D198: bench-only content hashing for average-policy snapshots.
// FNV-1a 64-bit over the flat avg_sigma vectors in deterministic DFS node
// order, combo-row-major, per action — the serialization pinned by the
// Averaging-Policy Re-Solve Addendum so hashes are reproducible across
// processes. No production surface; lives entirely in bigshark_rfc0047_bench.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bs::rfc0047 {

// Hash one full RiverSolveResult-style snapshot table: outer vector indexed
// by DFS tree node (empty entries skipped in order), each non-empty row
// hashed byte-for-byte after its node index, so two tables with the same
// contents hash equal and any content change changes the hash (modulo the
// 2^-64 FNV collision probability).
std::uint64_t hash_average_snapshot(const std::vector<std::vector<double>>& avg_sigma);

// Per-combo row L1 distance d = sum_a |p(a) - q(a)|; both rows describe one
// combo's menu-width distribution and must have equal width.
double row_l1_distance(const std::vector<double>& p, const std::vector<double>& q);

// Maximum per-combo row L1 over two equal-shape snapshot tables: at each
// non-empty node, each kC combo row has menu = row.size()/kC entries, and the
// returned value is the largest combo-level d (bounded in [0,2]), matching
// the RFC A2 definition rather than a node-vector aggregate.
double max_snapshot_l1(const std::vector<std::vector<double>>& linear,
                       const std::vector<std::vector<double>>& uniform);

}  // namespace bs::rfc0047
