// RFC 0031 Stage 0: flop-library segmentation statistics.
//
// Reproduces the five baseline statistics that motivate the card-abstraction
// measurement, and the trained/untrained segmented table, directly from a
// packed BSFLIB02 library. The definitions are the RFC 0031 ones:
//
//   public nodes        distinct node_hash values across every class section
//   bucket rows         sum of per-node bucket row counts
//   distinct vectors/n  distinct decoded probability sequences per node,
//                       averaged over the node population
//   rows / distinct     total bucket rows divided by total distinct sequences
//   untrained node      a node whose EVERY bucket row is uniform over the menu
//
// A row is uniform when every decoded probability is within `uniform_eps` of
// 1/menu_size; averaged-strategy normalization leaves FP noise (a 3-way menu
// stores 0.333333...), so exact equality undercounts genuinely untrained rows.
// 1e-12 is below the empirical gap between normalized noise and learned mass
// and reproduces the RFC 0031 pinned baseline exactly.
constexpr double kUniformEps = 1e-12;
//
// Usage: bigshark-flop-library-stats [library.bin] [uniform-eps]
//   library.bin   defaults to artifacts/flop-library-6way-fullrange/library.bin
//   uniform-eps   absolute tolerance for the uniform test (default 1e-12)
#include <bs/mmap_library.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

#include "resident/resident_index.hpp"

namespace fs = std::filesystem;
using bs::resident::CompactRowView;
using bs::resident::MmapLibrary;
using bs::resident::ProbabilityLayout;

namespace {

double decode_prob(const CompactRowView& row, std::size_t i) {
  switch (row.prob_layout) {
    case ProbabilityLayout::Codebook8:
      return row.codebook[row.probability_indices[i]];
    case ProbabilityLayout::Codebook16:
      return row.codebook[row.probability_indices_16[i]];
    case ProbabilityLayout::Doubles:
      return row.probabilities[i];
  }
  return 0.0;
}

bool is_uniform(const CompactRowView& row, double eps) {
  const double target = 1.0 / static_cast<double>(row.count);
  for (std::uint16_t i = 0; i < row.count; ++i) {
    if (std::fabs(decode_prob(row, i) - target) > eps)
      return false;
  }
  return true;
}

// Bit-for-bit identity of two decoded probability sequences.
bool same_vector(const CompactRowView& a, const CompactRowView& b) {
  if (a.count != b.count)
    return false;
  for (std::uint16_t i = 0; i < a.count; ++i) {
    if (decode_prob(a, i) != decode_prob(b, i))
      return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const fs::path path =
      argc > 1 ? fs::path(argv[1]) : fs::path("artifacts/flop-library-6way-fullrange/library.bin");
  const double eps = argc > 2 ? std::strtod(argv[2], nullptr) : kUniformEps;

  MmapLibrary library;
  try {
    library.open(path, /*verify_sha256=*/true);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "failed to open %s: %s\n", path.string().c_str(), error.what());
    return 1;
  }

  std::uint64_t total_nodes = 0;
  std::uint64_t total_rows = 0;
  std::uint64_t total_distinct = 0;
  std::uint64_t untrained_nodes = 0;

  std::uint64_t trained_nodes = 0;
  std::uint64_t trained_rows = 0;
  std::uint64_t trained_distinct = 0;
  std::uint64_t learned_rows = 0;
  std::uint64_t uniform_rows = 0;
  std::uint64_t single_learned_nodes = 0;

  std::unordered_set<std::uint64_t> distinct_hashes;

  const std::size_t classes = library.class_count();
  for (std::size_t ci = 0; ci < classes; ++ci) {
    const std::uint32_t class_id = library.class_id_at(ci);
    const std::size_t nodes = library.node_count(class_id);
    for (std::size_t n = 0; n < nodes; ++n) {
      const std::size_t rows = library.node_bucket_count_at(class_id, n);
      std::vector<CompactRowView> views(rows);
      std::vector<bool> uniform_row(rows, false);
      std::uint64_t learned_here = 0;
      for (std::size_t r = 0; r < rows; ++r) {
        library.node_bucket_row_at(class_id, n, r, views[r]);
        uniform_row[r] = is_uniform(views[r], eps);
        if (uniform_row[r])
          ++uniform_rows;
        else {
          ++learned_rows;
          ++learned_here;
        }
      }

      // Distinct decoded sequences among the node's rows: O(k^2); k is small
      // (at most the off-board combo count), so no hashing of doubles needed.
      std::uint64_t distinct = 0;
      for (std::size_t r = 0; r < rows; ++r) {
        bool unique = true;
        for (std::size_t prev = 0; prev < r; ++prev) {
          if (same_vector(views[r], views[prev])) {
            unique = false;
            break;
          }
        }
        if (unique)
          ++distinct;
      }

      ++total_nodes;
      total_rows += rows;
      total_distinct += distinct;
      distinct_hashes.insert(library.node_hash_at(class_id, n));

      if (learned_here == 0) {
        ++untrained_nodes;
      } else {
        ++trained_nodes;
        trained_rows += rows;
        trained_distinct += distinct;
        if (learned_here == 1)
          ++single_learned_nodes;
      }
    }
  }

  std::printf("library                    : %s\n", path.string().c_str());
  std::printf("sha256                     : %s\n", library.sha256_hex().c_str());
  std::printf("classes                    : %zu\n", classes);
  std::printf("uniform test epsilon       : %.3g\n", eps);
  std::printf("\n");
  std::printf("public nodes (sum)         : %llu\n", static_cast<unsigned long long>(total_nodes));
  std::printf("public nodes (distinct)    : %llu\n",
              static_cast<unsigned long long>(distinct_hashes.size()));
  std::printf("bucket rows                : %llu\n", static_cast<unsigned long long>(total_rows));
  std::printf("distinct vectors/node      : %.4f\n",
              static_cast<double>(total_distinct) / static_cast<double>(total_nodes));
  std::printf("rows / distinct vector     : %.4f\n",
              static_cast<double>(total_rows) / static_cast<double>(total_distinct));
  std::printf("untrained nodes            : %llu (%.2f%%)\n",
              static_cast<unsigned long long>(untrained_nodes),
              100.0 * static_cast<double>(untrained_nodes) / static_cast<double>(total_nodes));
  std::printf("\n");
  std::printf("trained nodes              : %llu\n",
              static_cast<unsigned long long>(trained_nodes));
  std::printf("rows at trained nodes      : %llu\n", static_cast<unsigned long long>(trained_rows));
  std::printf("distinct vectors/trained   : %.4f\n",
              static_cast<double>(trained_distinct) / static_cast<double>(trained_nodes));
  std::printf("rows/distinct (trained)    : %.4f\n",
              static_cast<double>(trained_rows) / static_cast<double>(trained_distinct));
  std::printf("rows per trained node      : %.4f\n",
              static_cast<double>(trained_rows) / static_cast<double>(trained_nodes));
  std::printf("learned rows               : %llu (%.4f/trained node)\n",
              static_cast<unsigned long long>(learned_rows),
              static_cast<double>(learned_rows) / static_cast<double>(trained_nodes));
  // Uniform rows located at trained nodes (untrained-node rows are uniform by
  // construction and counted separately).
  const std::uint64_t uniform_rows_at_trained = uniform_rows - (total_rows - trained_rows);
  std::printf("uniform rows/trained node  : %.4f\n",
              static_cast<double>(uniform_rows_at_trained) / static_cast<double>(trained_nodes));
  std::printf(
      "single-learned nodes       : %llu (%.1f%% of trained)\n",
      static_cast<unsigned long long>(single_learned_nodes),
      100.0 * static_cast<double>(single_learned_nodes) / static_cast<double>(trained_nodes));
  return 0;
}
