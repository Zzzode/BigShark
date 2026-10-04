// main.cpp — engine host entry point.
//   default:        one decision, JSON context on stdin -> JSON decision on stdout
//   --serve:        persistent line-delimited coprocess: read one JSON object per
//                   stdin line, print one JSON decision per stdout line (flush each).
//   --proto:        one RFC 0002 framed Envelope on stdin -> one framed Envelope
//   --serve-proto:  persistent framed Protobuf coprocess, one frame in/out per
//                   decision. Diagnostics go to stderr; stdout carries frames only.
//
// RFC 0002 Stage 8 offline provisioning (proto modes only):
//   --resident-root <path>=<sha256-hex>
//                   Repeatable. Loads and digest-pins one immutable resident
//                   blueprint artifact at startup. With no flags the host is
//                   the exact default binary: no resident roots, BLUEPRINT
//                   unadvertised, and minor 0/v0/JSON paths unchanged. One bad
//                   root never disables another; per-root load results are
//                   reported on stderr only, never on framed stdout.
//   --flop-library <dir>
//                   Repeatable. Reads <dir>/manifest.json and expands each
//                   class entry into a resident root spec (path + sha256
//                   pin), loading the entire flop class library in one flag.
//                   The manifest is written by the flop library builder
//                   (engine/src/flop_library/flop_library.cpp). A missing
//                   directory, unparseable manifest, or bad sha256 aborts
//                   startup with exit code 2; per-root load failures are
//                   reported individually and never disable another root.
//   --resident-budget <MiB>
//                   Total resident budget shared across all advertised roots.
//                   Defaults to 256 MiB (bs::resident::kDefaultResidentBudgetBytes).
//                   A full flop class library needs ~6 GiB (6144 MiB); below
//                   the library's footprint the engine advertises only the
//                   classes that fit and reports the rest OverBudget. A
//                   missing value, non-numeric value, zero, or overflow aborts
//                   startup with exit code 2.
#include <third_party/yyjson/yyjson.h>

#include <array>
#include <bs/decision.hpp>
#include <bs/resident_policy.hpp>
#include <bs/service.hpp>
#include <bs/strategy_artifact.hpp>
#include <bs/v0_protocol.hpp>
#include <bs/v1_protocol.hpp>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bs::artifacts::Sha256Digest;

bool parseSha256Hex(std::string_view text, Sha256Digest& digest) {
  if (text.size() != 64)
    return false;
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < digest.size(); ++i) {
    const int high = nibble(text[2 * i]);
    const int low = nibble(text[2 * i + 1]);
    if (high < 0 || low < 0)
      return false;
    digest[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

// V1HostServices composition root. Owns the immutable resident set and the
// per-process lookup scratch; the frame loop is single threaded, so one
// scratch is reused for every blueprint decision.
class ResidentHostServices final : public bs::v1::V1HostServices {
 public:
  explicit ResidentHostServices(bs::resident::ResidentPolicySet set) : set_(std::move(set)) {}

  bool blueprintAdvertised() const noexcept override { return set_.advertised_roots() > 0; }

  bool resolvingAdvertised() const noexcept override { return set_.advertised_roots() > 0; }

  bs::v1::V1BlueprintResult blueprintHeroDecision(
      const bs::poker::GameState& state, std::span<const bs::poker::PublicAction> history,
      const std::array<int, 2>& hero_cards,
      std::string_view pinned_sha256) const noexcept override {
    bs::v1::V1BlueprintResult result;
    try {
      const bs::resident::ResidentAnswer answer =
          set_.hero_decision(state, history, hero_cards,
                             pinned_sha256.empty() ? std::optional<std::string_view>{}
                                                   : std::optional<std::string_view>{pinned_sha256},
                             scratch_);
      if (!answer.hit) {
        result.hit = false;
        result.miss = mapMiss(answer.reason);
        return result;
      }
      result.hit = true;
      result.miss = bs::v1::V1BlueprintMiss::None;
      result.row.size = answer.hero_row.size;
      result.row.actions = answer.hero_row.actions;
      result.row.probabilities = answer.hero_row.probabilities;
      result.row.artifact_sha256 = answer.artifact_sha256;
      return result;
    } catch (const std::exception&) {
      // Reconstruction already validated the node; any resident-side throw is
      // a deterministic coverage miss, never a fabricated policy.
      result.hit = false;
      result.miss = bs::v1::V1BlueprintMiss::OffTree;
      return result;
    }
  }

  bs::v1::V1ResolveResult resolvingDecision(const bs::poker::GameState& state,
                                            std::span<const bs::poker::PublicAction> history,
                                            const std::array<int, 2>& hero_cards,
                                            std::string_view pinned_sha256,
                                            std::uint32_t deadline_ms) const noexcept override {
    bs::v1::V1ResolveResult result;
    try {
      auto source = set_.resolver_source(state, history,
                                         pinned_sha256.empty()
                                             ? std::optional<std::string_view>{}
                                             : std::optional<std::string_view>{pinned_sha256});
      if (!source) {
        result.outcome = bs::v1::V1ResolveOutcome::Unsupported;
        result.miss = bs::v1::V1BlueprintMiss::RootNotSupported;
        return result;
      }

      // Resolve the baseline row FIRST, inside the request budget. The lookup
      // is a warm in-memory read (measured p99 tens of microseconds), so
      // taking it before training keeps it within the same receipt-anchored
      // window instead of running untimed after the solver consumed the whole
      // budget. A miss here is not fatal: the certified path may still succeed.
      bs::v1::V1ResolveResult baseline_result;
      bool have_baseline = false;
      {
        const bs::resident::ResidentAnswer answer = set_.hero_decision(
            state, history, hero_cards,
            pinned_sha256.empty() ? std::optional<std::string_view>{}
                                  : std::optional<std::string_view>{pinned_sha256},
            scratch_);
        if (answer.hit) {
          have_baseline = true;
          baseline_result.outcome = bs::v1::V1ResolveOutcome::DeadlineBlueprint;
          baseline_result.miss = bs::v1::V1BlueprintMiss::None;
          baseline_result.row.size = answer.hero_row.size;
          baseline_result.row.actions = answer.hero_row.actions;
          baseline_result.row.probabilities = answer.hero_row.probabilities;
          baseline_result.row.artifact_sha256 = answer.artifact_sha256;
        }
      }

      bs::resolver::ResolveLimits limits;
      limits.time = std::chrono::milliseconds(std::max<std::uint32_t>(deadline_ms, 1));
      // The request's public budget bounds the training work itself, not just
      // the wall-clock guard. The iteration cap is derived inside
      // Resolver::resolve once the joint-deal and action counts are known
      // (they are the cost drivers), from this budget alone; the configured
      // count stays the upper bound. The derivation reads only public inputs,
      // so every counterfactual hero combination derives the identical cap.
      limits.public_seed = publicSeed(*source, state, history);
      resolve_scratch_ = resolver_.resolve(state, history, *source, limits);

      const auto baseline = [&]() -> bool {
        if (!have_baseline)
          return false;
        result = baseline_result;
        return true;
      };

      switch (resolve_scratch_.status) {
        case bs::resolver::ResolveStatus::Certified: {
          const auto key =
              bs::solver::make_information_key(*state.actor(), hero_cards, state.board(), history);
          const auto found = resolve_scratch_.candidate.find(key);
          if (found == resolve_scratch_.candidate.end())
            return {bs::v1::V1ResolveOutcome::DeadlineExceeded,
                    bs::v1::V1BlueprintMiss::UntrainedCombo,
                    {}};
          result.outcome = bs::v1::V1ResolveOutcome::Certified;
          result.miss = bs::v1::V1BlueprintMiss::None;
          result.row.size = found->second.probabilities.size();
          result.row.actions = found->second.actions.data();
          result.row.probabilities = found->second.probabilities.data();
          result.row.artifact_sha256 = source->artifact_digest();
          // A multiway (3-seat) certification is a per-seat unilateral
          // non-regression bound, not a two-player equilibrium bound. The
          // diagnostic token carries that distinction on the wire; a two-seat
          // resolve leaves it empty so the frozen two-seat bytes are unchanged.
          if (resolve_scratch_.seat_count >= 3)
            result.row.diagnostic = bs::resolver::kMultiwayCertificationToken;
          return result;
        }
        case bs::resolver::ResolveStatus::SolveDeadline:
        case bs::resolver::ResolveStatus::CertifyDeadline:
        case bs::resolver::ResolveStatus::CertificationRejected:
          if (baseline())
            return result;
          result.outcome = bs::v1::V1ResolveOutcome::DeadlineExceeded;
          return result;
        case bs::resolver::ResolveStatus::Ineligible:
          result.outcome = bs::v1::V1ResolveOutcome::Unsupported;
          result.miss = bs::v1::V1BlueprintMiss::RootNotSupported;
          return result;
        case bs::resolver::ResolveStatus::CoverageMiss:
          result.outcome = bs::v1::V1ResolveOutcome::Unsupported;
          result.miss = bs::v1::V1BlueprintMiss::OffTree;
          return result;
        case bs::resolver::ResolveStatus::InvalidInput:
          result.outcome = bs::v1::V1ResolveOutcome::Unsupported;
          result.miss = bs::v1::V1BlueprintMiss::OffTree;
          return result;
      }
    } catch (const std::exception&) {
      result.outcome = bs::v1::V1ResolveOutcome::Unsupported;
      result.miss = bs::v1::V1BlueprintMiss::OffTree;
    }
    return result;
  }

 private:
  // Deterministic public-context seed (FNV-1a over the digest, root identity,
  // board, and exact public history). It never reads the actual hero hand.
  //
  // The mix order and field bytes are frozen: at two seats it is byte-for-byte
  // the former HeadsUpRoot/HeadsUpState seed (root.flop, the two stacks, the
  // two contributions, pot, button, board span, then street/actor/type/target
  // per history event), so a certified two-seat artifact's seed does not move.
  // GameDef.board carries the flop in its first three ints, and a PublicAction
  // shares the BettingEvent byte layout for the three mixed fields.
  static std::uint64_t publicSeed(const bs::resolver::BlueprintSource& source,
                                  const bs::poker::GameState& state,
                                  std::span<const bs::poker::PublicAction> history) {
    std::uint64_t hash = 1469598103934665603ULL;
    auto mix = [&hash](const void* data, std::size_t bytes) {
      const auto* bytes_ptr = static_cast<const unsigned char*>(data);
      for (std::size_t i = 0; i < bytes; ++i) {
        hash ^= bytes_ptr[i];
        hash *= 1099511628211ULL;
      }
    };
    const std::string digest(source.artifact_digest());
    mix(digest.data(), digest.size());
    const auto& def = state.def();
    mix(def.board.data(), 3 * sizeof(int));
    mix(def.stacks.data(), def.player_count * sizeof(bs::poker::Chips));
    mix(def.contributions.data(), def.player_count * sizeof(bs::poker::Chips));
    mix(&def.pot, sizeof(def.pot));
    mix(&def.button, sizeof(def.button));
    mix(state.board().data(), state.board().size() * sizeof(int));
    for (const auto& event : history) {
      mix(&event.street, sizeof(event.street));
      mix(&event.seat, sizeof(event.seat));
      mix(&event.action.type, sizeof(event.action.type));
      mix(&event.action.target_total, sizeof(event.action.target_total));
    }
    return hash;
  }

  static bs::v1::V1BlueprintMiss mapMiss(bs::resident::MissReason reason) {
    using bs::resident::MissReason;
    switch (reason) {
      case MissReason::None:
        return bs::v1::V1BlueprintMiss::None;
      case MissReason::RootNotSupported:
        return bs::v1::V1BlueprintMiss::RootNotSupported;
      case MissReason::RootIdentityMismatch:
        return bs::v1::V1BlueprintMiss::RootIdentityMismatch;
      case MissReason::OverBudgetNotAdvertised:
        return bs::v1::V1BlueprintMiss::OverBudgetNotAdvertised;
      case MissReason::MissingHistory:
        return bs::v1::V1BlueprintMiss::MissingHistory;
      case MissReason::OffTree:
        return bs::v1::V1BlueprintMiss::OffTree;
      case MissReason::OffTreeAmount:
        return bs::v1::V1BlueprintMiss::OffTreeAmount;
      case MissReason::ZeroProbabilityObservedAction:
        return bs::v1::V1BlueprintMiss::ZeroProbabilityObservedAction;
      case MissReason::EmptyJointRange:
        return bs::v1::V1BlueprintMiss::EmptyJointRange;
      case MissReason::UntrainedCombo:
        return bs::v1::V1BlueprintMiss::UntrainedCombo;
      case MissReason::ComboBlockedByBoard:
        return bs::v1::V1BlueprintMiss::ComboBlockedByBoard;
      case MissReason::RunoutDivergence:
        return bs::v1::V1BlueprintMiss::RunoutDivergence;
      case MissReason::OpponentRangeFullyBlocked:
        return bs::v1::V1BlueprintMiss::OpponentRangeFullyBlocked;
      case MissReason::ZeroProbabilityHeroCombination:
        return bs::v1::V1BlueprintMiss::ZeroProbabilityHeroCombination;
      case MissReason::SeatCountNotSupported:
        return bs::v1::V1BlueprintMiss::SeatCountNotSupported;
    }
    return bs::v1::V1BlueprintMiss::OffTree;
  }

  bs::resident::ResidentPolicySet set_;
  mutable bs::resident::ResidentScratch scratch_;
  // One resolver and its per-request whole-range result. The frame loop is
  // single threaded; the selected row pointers reference resolve_scratch_ and
  // are consumed synchronously by the response mapper.
  mutable bs::resolver::Resolver resolver_;
  mutable bs::resolver::ResolveResult resolve_scratch_;
};

// Parses --resident-root path=sha256-hex entries. Returns false on a malformed
// spec; the caller aborts before serving.
bool collectResidentSpecs(int argc, char** argv,
                          std::vector<bs::resident::SupportedRootSpec>& specs) {
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument != "--resident-root")
      continue;
    if (i + 1 >= argc)
      return false;
    const std::string spec_text = argv[++i];
    const std::size_t separator = spec_text.rfind('=');
    if (separator == std::string::npos || separator == 0 || separator + 1 + 64 != spec_text.size())
      return false;
    Sha256Digest digest{};
    if (!parseSha256Hex(std::string_view(spec_text).substr(separator + 1), digest))
      return false;
    specs.push_back({spec_text.substr(0, separator), digest});
  }
  return true;
}

// Parses --resident-budget <MiB>. Last occurrence wins. Returns the budget in
// bytes on success, std::nullopt when the flag is absent, and false on a
// missing value, non-numeric value, zero, or a value that overflows size_t
// when converted to bytes.
std::optional<std::size_t> collectResidentBudget(int argc, char** argv) {
  std::optional<std::size_t> budget_bytes;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument != "--resident-budget")
      continue;
    if (i + 1 >= argc)
      return std::nullopt;
    const std::string_view text = argv[++i];
    std::uint64_t mib = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, mib);
    if (ec != std::errc{} || ptr != end || mib == 0)
      return std::nullopt;
    constexpr std::uint64_t max_mib =
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() >> 20);
    if (mib > max_mib)
      return std::nullopt;
    budget_bytes = static_cast<std::size_t>(mib) << 20;
  }
  return budget_bytes;
}

// Expands --flop-library <dir> flags into resident root specs. Reads
// <dir>/manifest.json (written by the flop library builder) and appends one
// SupportedRootSpec per class entry: path = <dir>/<artifact_name>, sha256 =
// the entry's pinned digest. Returns false on any startup configuration error
// (missing directory, unparseable manifest, bad sha256); per-root load
// failures are handled later by ResidentPolicySet::build and never abort.
bool collectFlopLibrarySpecs(int argc, char** argv,
                             std::vector<bs::resident::SupportedRootSpec>& specs) {
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument != "--flop-library")
      continue;
    if (i + 1 >= argc)
      return false;
    const std::filesystem::path dir = argv[++i];
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
      std::cerr << "flop-library: not a directory: " << dir << '\n';
      return false;
    }
    const std::filesystem::path manifest_path = dir / "manifest.json";
    std::ifstream manifest_file(manifest_path, std::ios::binary);
    if (!manifest_file) {
      std::cerr << "flop-library: cannot open manifest: " << manifest_path << '\n';
      return false;
    }
    std::string content((std::istreambuf_iterator<char>(manifest_file)),
                        std::istreambuf_iterator<char>());

    yyjson_doc* doc = yyjson_read(content.data(), content.size(), 0);
    if (!doc) {
      std::cerr << "flop-library: manifest is not valid JSON: " << manifest_path << '\n';
      return false;
    }
    struct DocGuard {
      yyjson_doc* d;
      ~DocGuard() { yyjson_doc_free(d); }
    } guard{doc};
    yyjson_val* root = yyjson_doc_get_root(doc);
    yyjson_val* classes = yyjson_is_obj(root) ? yyjson_obj_get(root, "classes") : nullptr;
    if (!yyjson_is_arr(classes) || yyjson_arr_size(classes) == 0) {
      std::cerr << "flop-library: manifest has no non-empty classes array: " << manifest_path
                << '\n';
      return false;
    }
    const std::size_t count = yyjson_arr_size(classes);
    for (std::size_t c = 0; c < count; ++c) {
      yyjson_val* entry = yyjson_arr_get(classes, c);
      yyjson_val* name_val =
          yyjson_is_obj(entry) ? yyjson_obj_get(entry, "artifact_name") : nullptr;
      yyjson_val* sha_val = yyjson_is_obj(entry) ? yyjson_obj_get(entry, "sha256_hex") : nullptr;
      if (!yyjson_is_str(name_val) || yyjson_get_len(name_val) == 0) {
        std::cerr << "flop-library: class " << c << " missing or empty artifact_name\n";
        return false;
      }
      const std::string artifact_name = yyjson_get_str(name_val);
      if (artifact_name == "." || artifact_name == ".." ||
          artifact_name.find_first_of("/\\") != std::string::npos) {
        std::cerr << "flop-library: class " << c << " artifact_name must be a bare filename\n";
        return false;
      }
      if (!yyjson_is_str(sha_val)) {
        std::cerr << "flop-library: class " << c << " (" << artifact_name
                  << ") missing sha256_hex\n";
        return false;
      }
      Sha256Digest digest{};
      if (!parseSha256Hex(yyjson_get_str(sha_val), digest)) {
        std::cerr << "flop-library: class " << c << " (" << artifact_name
                  << ") invalid sha256_hex\n";
        return false;
      }
      specs.push_back({dir / artifact_name, digest});
    }
  }
  return true;
}

std::unique_ptr<ResidentHostServices> buildResidentServices(
    const std::vector<bs::resident::SupportedRootSpec>& specs, std::size_t budget_bytes) {
  if (specs.empty())
    return nullptr;
  std::vector<bs::resident::RootLoadResult> results;
  bs::resident::ResidentOptions options;
  options.budget_bytes = budget_bytes;
  bs::resident::ResidentPolicySet set =
      bs::resident::ResidentPolicySet::build(specs, options, &results);
  std::cerr << "resident-budget " << (budget_bytes >> 20) << " MiB\n";
  for (const bs::resident::RootLoadResult& result : results) {
    std::cerr << "resident-root " << result.path << " " << bs::resident::to_string(result.status);
    if (!result.sha256_hex.empty())
      std::cerr << " sha256=" << result.sha256_hex;
    if (result.status == bs::resident::RootStatus::Advertised) {
      std::cerr << " information_sets=" << result.information_sets
                << " resident_bytes=" << result.resident_bytes;
    }
    if (!result.detail.empty())
      std::cerr << " detail=" << result.detail;
    std::cerr << '\n';
  }
  return std::make_unique<ResidentHostServices>(std::move(set));
}

}  // namespace

static std::string decideJson(const std::string& raw) {
  bs::Decision d;
  try {
    bs::Ctx ctx = bs::v0::parseRequest(raw);
    d = bs::decide(ctx);
  } catch (const std::exception&) {
    return "{\"action\":\"fold\",\"amount\":0,\"reason\":\"parse-error\"}";
  }
  return bs::v0::serializeResponse(d);
}

// Handles one frame at a time and stops on a framing-level protocol error.
static int runProto(std::istream& input, std::ostream& output, bool persistent,
                    const bs::v1::V1HostServices* services) {
  bs::v1::FrameReader reader(input);
  std::string frame;
  while (true) {
    frame.clear();
    const bs::v1::FrameReader::Status status = reader.read(frame);
    if (status == bs::v1::FrameReader::Status::EndOfStream)
      return 0;
    if (status != bs::v1::FrameReader::Status::Complete) {
      std::cerr << "v1 frame protocol error\n";
      return 2;
    }

    const bs::v1::EnvelopeResult result =
        services ? bs::v1::handleEnvelope(frame, *services) : bs::v1::handleEnvelope(frame);
    if (result.outcome != bs::v1::EnvelopeOutcome::Respond)
      return 2;
    std::string encoded;
    if (!bs::v1::writeFrame(encoded, result.response)) {
      // An over-large engine response is an engine fault, never a reason to
      // kill a persistent coprocess: report on stderr and keep serving.
      std::cerr << "v1 response exceeded frame limit\n";
      continue;
    }
    output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    output.flush();

    if (!persistent)
      return 0;
  }
}

int main(int argc, char** argv) {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);

  bool serve = false;
  bool proto = false;
  bool serveProto = false;
  for (int i = 1; i < argc; i++) {
    const std::string argument = argv[i];
    if (argument == "--serve")
      serve = true;
    else if (argument == "--proto")
      proto = true;
    else if (argument == "--serve-proto")
      serveProto = true;
  }

  if (proto || serveProto) {
    std::vector<bs::resident::SupportedRootSpec> resident_specs;
    if (!collectResidentSpecs(argc, argv, resident_specs)) {
      std::cerr << "invalid --resident-root spec; expected <path>=<64 lowercase hex sha256>\n";
      return 2;
    }
    if (!collectFlopLibrarySpecs(argc, argv, resident_specs)) {
      std::cerr << "invalid --flop-library spec; expected <dir> with a valid manifest.json\n";
      return 2;
    }
    const std::optional<std::size_t> resident_budget = collectResidentBudget(argc, argv);
    if (!resident_budget) {
      // collectResidentBudget returns nullopt both when the flag is absent
      // (use the default) and when it is present but invalid (abort). Re-scan
      // to distinguish the two.
      for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--resident-budget") {
          std::cerr << "invalid --resident-budget; expected a positive MiB value\n";
          return 2;
        }
      }
    }
    std::unique_ptr<ResidentHostServices> services;
    try {
      services = buildResidentServices(
          resident_specs, resident_budget.value_or(bs::resident::kDefaultResidentBudgetBytes));
    } catch (const std::exception& error) {
      std::cerr << "resident startup failed: " << error.what() << '\n';
      return 2;
    }
    return runProto(std::cin, std::cout, serveProto, services.get());
  }

  if (serve) {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.empty())
        continue;
      std::cout << decideJson(line) << '\n' << std::flush;
    }
    return 0;
  }

  std::ostringstream ss;
  ss << std::cin.rdbuf();
  std::cout << decideJson(ss.str()) << '\n';
  return 0;
}
