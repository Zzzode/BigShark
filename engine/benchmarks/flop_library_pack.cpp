// RFC 0012 Phase 2: offline pack tool.
//
// Converts a directory of schema-v3 SQLite class files into a single
// consolidated binary file for mmap loading. The SQLite files remain the
// canonical format; the .bin is a derived, cacheable acceleration layer.
//
// Usage: bigshark-flop-library-pack <input-dir> <output.bin>
//
// The input directory must contain class-NNNN.db files (as produced by
// bigshark-flop-library-builder). The shared game identity (player count,
// blinds, stacks, contributions, pot, terminal depth, sizing schedule) is
// taken from the first artifact; every artifact must share it. The output
// is <output.bin> plus <output.bin>.sha256.
#include <algorithm>
#include <bs/mmap_library.hpp>
#include <bs/strategy_artifact.hpp>
#include <bs/unified_game.hpp>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <vector>
namespace fs = std::filesystem;

namespace {

// Extract the numeric class index from a "class-NNNN.db" filename.
// Returns ~0u when the filename does not match the pattern.
std::uint32_t class_id_from_filename(const fs::path& path) {
  const std::string name = path.filename().string();
  if (name.rfind("class-", 0) != 0 || name.size() < 12)
    return ~0u;
  const std::string digits = name.substr(6, name.size() - 9);  // strip "class-" and ".db"
  try {
    return static_cast<std::uint32_t>(std::stoul(digits));
  } catch (...) {
    return ~0u;
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <input-dir> <output.bin>\n", argv[0]);
    return 2;
  }

  const fs::path input_dir = argv[1];
  const fs::path output_path = argv[2];

  if (!fs::is_directory(input_dir)) {
    std::fprintf(stderr, "error: %s is not a directory\n", input_dir.string().c_str());
    return 1;
  }

  // Collect class-*.db files, sorted by filename.
  std::vector<fs::path> artifact_paths;
  for (const auto& entry : fs::directory_iterator(input_dir)) {
    if (!entry.is_regular_file())
      continue;
    const std::string name = entry.path().filename().string();
    if (name.rfind("class-", 0) == 0 && name.size() >= 12 && name.substr(name.size() - 3) == ".db")
      artifact_paths.push_back(entry.path());
  }
  std::sort(artifact_paths.begin(), artifact_paths.end());

  if (artifact_paths.empty()) {
    std::fprintf(stderr, "error: no class-*.db files found in %s\n", input_dir.string().c_str());
    return 1;
  }

  std::printf("packing %zu class artifacts from %s (streaming)\n", artifact_paths.size(),
              input_dir.string().c_str());

  // Load the first artifact to extract the shared game identity.
  bs::artifacts::LoadedArtifact first;
  try {
    first = bs::artifacts::load_artifact(artifact_paths[0]);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: failed to load %s: %s\n", artifact_paths[0].string().c_str(),
                 e.what());
    return 1;
  }
  if (!first.bundle.nseat) {
    std::fprintf(stderr, "error: %s is not a schema-v2/v3 seat-generic artifact\n",
                 artifact_paths[0].string().c_str());
    return 1;
  }
  const auto& first_policy = first.bundle.nseat->policy;
  if (first_policy.game().board_size != 3) {
    std::fprintf(stderr, "error: %s is not flop-rooted (board_size=%zu)\n",
                 artifact_paths[0].string().c_str(),
                 static_cast<std::size_t>(first_policy.game().board_size));
    return 1;
  }

  bs::solver::UnifiedGame game;
  game.def = first_policy.game();
  game.sizes = first_policy.sizes();

  // Streaming supplier: loads one artifact at a time. The returned
  // PackClassInput borrows pointers into `current`; they are valid until the
  // next call to the supplier.
  bs::artifacts::LoadedArtifact current;
  auto supplier = [&](std::size_t index) -> bs::resident::PackClassInput {
    try {
      current = bs::artifacts::load_artifact(artifact_paths[index]);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "error: failed to load %s: %s\n", artifact_paths[index].string().c_str(),
                   e.what());
      std::exit(1);
    }
    if (!current.bundle.nseat) {
      std::fprintf(stderr, "error: %s is not a schema-v2/v3 seat-generic artifact\n",
                   artifact_paths[index].string().c_str());
      std::exit(1);
    }
    const auto& policy = current.bundle.nseat->policy;
    if (policy.game().board_size != 3) {
      std::fprintf(stderr, "error: %s is not flop-rooted (board_size=%zu)\n",
                   artifact_paths[index].string().c_str(),
                   static_cast<std::size_t>(policy.game().board_size));
      std::exit(1);
    }

    bs::resident::PackClassInput input;
    input.class_id = class_id_from_filename(artifact_paths[index]);
    if (input.class_id == ~0u)
      input.class_id = static_cast<std::uint32_t>(index);
    const auto& game_def = policy.game();
    input.canonical_board = {game_def.board[0], game_def.board[1], game_def.board[2]};
    input.rows = &policy.rows();
    if (!policy.ranges().empty())
      input.range = &policy.ranges()[0];
    return input;
  };

  // Pack the binary library using the streaming writer.
  try {
    bs::resident::pack_binary_library_streaming(output_path, game, artifact_paths.size(), supplier);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: pack failed: %s\n", e.what());
    return 1;
  }

  const auto file_size = fs::file_size(output_path);
  std::printf("packed %zu classes into %s (%.2f MB)\n", artifact_paths.size(),
              output_path.string().c_str(), static_cast<double>(file_size) / (1024.0 * 1024.0));
  return 0;
}
