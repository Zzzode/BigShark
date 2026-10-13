// Native coverage for template reuse, single-flight loads, and lifetime budgets.
#include <array>
#include <barrier>
#include <bs/nseat_policy_codec.hpp>
#include <bs/postflop_config.hpp>
#include <bs/postflop_frontier.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <vector>

#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) {                                                        \
      std::printf("CHECK failed: %s:%d %s\n", __FILE__, __LINE__, #c); \
      return 1;                                                        \
    }                                                                  \
  } while (0)

namespace {
using namespace bs;
using namespace bs::gto;

struct Files {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("bs_postflop_cache_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Files() {
    if (!std::filesystem::create_directory(path))
      throw std::runtime_error("cannot create cache fixture directory");
  }
  ~Files() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

std::array<int, 3> canonical(std::array<int, 3> flop) {
  return abstraction::canonicalize(flop).board;
}

void save_policy(const Files& files, std::array<int, 3> flop) {
  flop = canonical(flop);
  const auto def = make_postflop_def(flop, 0);
  const auto action = make_postflop_abstraction(0);
  const auto policy = solver::NSeatPolicy::from_parts(def, action.id(), {});
  solver::save_nseat_policy(policy, 0, files.path / solver::policy_file_name(flop, 0));
}

std::vector<double> evaluate(OnTheFlyPostflopEvaluator& evaluator, std::array<int, 3> flop) {
  const auto def = make_postflop_def(flop, 0);
  const auto ledger = tree::make_frontier_payload(poker::GameState(def));
  const std::array<std::array<int, 2>, 2> holes{{{40, 51}, {41, 50}}};
  return evaluator.evaluate(flop, holes, ledger);
}

bool valid(const std::vector<double>& values) {
  return values.size() == 2 && std::isfinite(values[0]) && std::isfinite(values[1]) &&
         std::abs(values[0] + values[1]) < 1e-8;
}

int run() {
  Files files;
  const std::array<int, 3> first{0, 1, 2};
  const auto representative = make_postflop_def(first, 0);
  tree::AbstractTree tree(representative, make_postflop_abstraction(0));
  tree.save(files.path / tree::tree_file_name(first, 0));
  save_policy(files, first);
  NSeatDeclaredFrontierTable::Metadata metadata;
  metadata.representative_pot.fill(100.0);
  NSeatDeclaredFrontierTable table({}, metadata);
  OnTheFlyPostflopEvaluator evaluator(table, files.path, 1);

  // Eight simultaneous cold calls must share one policy and template load.
  std::barrier start(8);
  std::vector<std::future<std::vector<double>>> requests;
  for (int i = 0; i < 8; ++i)
    requests.push_back(std::async(std::launch::async, [&] {
      start.arrive_and_wait();
      return evaluate(evaluator, first);
    }));
  std::vector<double> reference;
  for (auto& request : requests) {
    // CTest supplies the process timeout. Always join before dependent objects
    // leave scope, including on exceptions or a failed assertion.
    auto value = request.get();
    CHECK(valid(value));
    if (reference.empty())
      reference = value;
    CHECK(value == reference);
  }
  auto stats = evaluator.statistics();
  CHECK(stats.policy_loads == 1 && stats.template_loads == 1);
  CHECK(stats.policy_bytes > 0 && stats.policy_bytes <= stats.peak_policy_bytes);
  CHECK(stats.peak_policy_bytes <= PostflopCacheLimits{}.max_policy_bytes);

  // Different boards retain independent policies, but share the same template.
  const std::array<int, 3> second{0, 5, 10};
  save_policy(files, second);
  CHECK(valid(evaluate(evaluator, second)));
  CHECK(evaluator.statistics().policy_loads == 2);
  CHECK(evaluator.statistics().template_loads == 1);
  CHECK(valid(evaluate(evaluator, first)));
  CHECK(evaluator.statistics().policy_loads == 3);

  // A failed flight releases its reservation and does not poison later retries.
  OnTheFlyPostflopEvaluator retry(table, files.path);
  const std::array<int, 3> repaired{0, 9, 14};
  bool failed = false;
  try {
    evaluate(retry, repaired);
  } catch (const std::runtime_error&) {
    failed = true;
  }
  CHECK(failed);
  CHECK(retry.statistics().policy_bytes == 0);
  save_policy(files, repaired);
  CHECK(valid(evaluate(retry, repaired)));
  CHECK(retry.statistics().policy_loads == 1);

  // An aggregate template cap fails before publishing and releases policy bytes.
  PostflopCacheLimits small_template;
  small_template.max_template_bytes = 1 << 20;
  OnTheFlyPostflopEvaluator capped(table, files.path, 1, small_template);
  failed = false;
  try {
    evaluate(capped, first);
  } catch (const tree::tree_resource_exhausted&) {
    failed = true;
  }
  CHECK(failed && capped.statistics().policy_bytes == 0);
  CHECK(capped.statistics().template_loads == 0);

  // Under a one-load budget, distinct concurrent requests must evict/wait safely.
  PostflopCacheLimits tight;
  tight.max_policy_bytes = tight.max_load_bytes = 64 << 10;
  OnTheFlyPostflopEvaluator bounded(table, files.path, 1, tight);
  std::barrier together(8);
  // This batch must be destroyed before its evaluator and rendezvous.
  std::vector<std::future<std::vector<double>>> bounded_requests;
  for (int i = 1; i <= 8; ++i) {
    const std::array<int, 3> flop{0, 4 * i + 1, 4 * i + 2};
    save_policy(files, flop);
    bounded_requests.push_back(std::async(std::launch::async, [&, flop] {
      together.arrive_and_wait();
      return evaluate(bounded, flop);
    }));
  }
  for (auto& request : bounded_requests) {
    CHECK(valid(request.get()));
  }
  stats = bounded.statistics();
  CHECK(stats.policy_loads == 8 && stats.template_loads == 1);
  CHECK(stats.peak_policy_bytes <= tight.max_policy_bytes);

  failed = false;
  try {
    OnTheFlyPostflopEvaluator invalid(table, files.path, 0);
  } catch (const std::invalid_argument&) {
    failed = true;
  }
  CHECK(failed);
  std::printf("postflop cache single-flight, lifetime budget, template reuse, and retry PASS\n");
  return 0;
}
}  // namespace

int main() {
  try {
    return run();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
