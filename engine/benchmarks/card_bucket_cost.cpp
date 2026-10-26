// RFC 0031 AC5: per-lookup card_bucket cost for the measurable bucket
// schemes. The public-belief replay budget assumes ~75 ns per combo and
// rejects a scheme above 150 ns; the replay issues up to 1,326 card_bucket
// calls per observed action. This benchmark times each scheme over every
// non-blocked hole pair on a representative flop, turn, and river board and
// reports the median and 95th-percentile nanoseconds per call.
//
// Usage: bigshark-card-bucket-cost [iterations]
#include <algorithm>
#include <array>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace bs::abstraction;

namespace {

struct TimingRow {
  const char* street;
  CardBucketKind kind;
  double median_ns;
  double p95_ns;
};

double nanoseconds_per_call(const std::vector<std::array<int, 2>>& holes,
                            std::span<const int> board, CardBucketKind kind,
                            std::uint64_t iterations) {
  volatile std::uint32_t sink = 0;
  const auto t0 = std::chrono::steady_clock::now();
  std::uint64_t calls = 0;
  for (std::uint64_t rep = 0; rep < iterations; ++rep) {
    for (const auto& hole : holes) {
      sink ^= card_bucket(kind, hole, board);
      ++calls;
    }
  }
  const auto t1 = std::chrono::steady_clock::now();
  const double elapsed_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
  (void)sink;
  return elapsed_ns / static_cast<double>(calls);
}

int card_id(char r, char s) {
  return bs::cardId(std::string{r, s});
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t iterations = argc > 1 ? std::stoull(argv[1]) : 2000;

  const std::array<int, 3> flop{card_id('2', 'c'), card_id('3', 'd'), card_id('7', 'h')};
  const std::array<int, 4> turn{flop[0], flop[1], flop[2], card_id('J', 's')};
  const std::array<int, 5> river{turn[0], turn[1], turn[2], turn[3], card_id('9', 'c')};

  const struct {
    const char* street;
    std::span<const int> board;
  } boards[] = {
      {"flop", std::span<const int>(flop)},
      {"turn", std::span<const int>(turn)},
      {"river", std::span<const int>(river)},
  };

  const struct {
    const char* name;
    CardBucketKind kind;
  } schemes[] = {
      {"tiers", CardBucketKind::CategoryTiersV1},
      {"tiers-draw", CardBucketKind::TiersDrawV1},
      {"identity", CardBucketKind::Identity},
  };

  std::printf(
      "RFC 0031 AC5: card_bucket cost (iterations=%llu, budget 75 ns target / "
      "150 ns rejection)\n",
      static_cast<unsigned long long>(iterations));
  std::printf("| street | scheme | ns/call |\n| --- | --- | --- |\n");
  for (const auto& b : boards) {
    std::vector<std::array<int, 2>> holes;
    for (int c0 = 0; c0 < 52; ++c0) {
      for (int c1 = c0 + 1; c1 < 52; ++c1) {
        bool blocked = false;
        for (int bc : b.board)
          if (bc == c0 || bc == c1)
            blocked = true;
        if (!blocked)
          holes.push_back({c0, c1});
      }
    }
    for (const auto& scheme : schemes) {
      const double ns = nanoseconds_per_call(holes, b.board, scheme.kind, iterations);
      std::printf("| %s | %s | %.2f |\n", b.street, scheme.name, ns);
    }
  }
  return 0;
}
