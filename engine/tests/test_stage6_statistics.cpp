// RFC 0008 stage 6 R8/R9: paired Student-t statistics for the estimator.
// Pins the t critical table, the unbiased variance, the scalar-sum CI rule,
// and the paired-difference interval (which must use within-pair variance,
// not the variance of either series).
#include <bs/stage6/statistics.hpp>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

using namespace bs::stage6;

namespace {
int failures = 0;
void check(bool c, const char* d) {
  if (!c) {
    std::fprintf(stderr, "FAIL: %s\n", d);
    ++failures;
  }
}
bool near(double a, double b, double tol) {
  return std::fabs(a - b) <= tol;
}
}  // namespace

int main() {
  // t critical values for a two-sided 95% interval (upper-tail 0.025).
  check(near(student_t_critical_95(1), 12.7062, 1e-4), "t(1) ~ 12.706");
  check(near(student_t_critical_95(2), 4.3027, 1e-4), "t(2) ~ 4.303");
  check(near(student_t_critical_95(3), 3.1824, 1e-4), "t(3) ~ 3.182");
  check(near(student_t_critical_95(10), 2.2281, 1e-4), "t(10) ~ 2.228");
  check(near(student_t_critical_95(30), 2.0423, 1e-4), "t(30) ~ 2.042");
  // Above the table the approximation approaches the 0.975 normal quantile
  // from above.
  check(student_t_critical_95(1000) > 1.95996 && student_t_critical_95(1000) < 1.97,
        "large-df t approaches 1.96 from above");
  bool threw = false;
  try {
    (void)student_t_critical_95(0);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  check(threw, "t(0) is rejected");

  // Unbiased sample variance: {2,4,4,4,5,5,7,9} has mean 5 and sample
  // variance 32/7.
  const std::vector<double> v = {2, 4, 4, 4, 5, 5, 7, 9};
  const SampleMoments m = sample_moments(v);
  check(m.n == 8, "moments count");
  check(near(m.mean, 5.0, 1e-12), "moments mean");
  check(near(m.sample_variance, 32.0 / 7.0, 1e-9), "unbiased sample variance");
  check(near(m.standard_error, std::sqrt(32.0 / 7.0 / 8.0), 1e-12), "standard error");

  // CI endpoints.
  const ConfidenceInterval ci = confidence_interval_95(v);
  check(ci.df == 7, "CI degrees of freedom");
  check(near(ci.mean, 5.0, 1e-12), "CI mean");
  check(near(ci.half_width, student_t_critical_95(7) * m.standard_error, 1e-12),
        "CI half width is t*SE");
  check(near(ci.lower, 5.0 - ci.half_width, 1e-12) && near(ci.upper, 5.0 + ci.half_width, 1e-12),
        "CI endpoints");

  // Fewer than two replicates cannot bound a mean.
  threw = false;
  try {
    (void)confidence_interval_95({3.0});
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  check(threw, "a single replicate yields no CI");

  // Scalar sum rule.
  check(near(sum_seats({0.1, -0.2, 0.4}), 0.3, 1e-12), "per-seat gains sum to the scalar");

  // Paired CI uses within-pair differences. Construct a = b + constant: the
  // difference must have zero variance and an exact CI of width zero, even
  // though each series alone has large variance — this is the property a
  // naive two-series test would get wrong.
  const std::vector<double> a = {10, 20, 10, 20, 10, 20};
  const std::vector<double> b = {7, 17, 7, 17, 7, 17};
  const ConfidenceInterval pci = paired_difference_ci_95(a, b);
  check(near(pci.mean, 3.0, 1e-12), "paired mean difference");
  check(near(pci.half_width, 0.0, 1e-12), "constant paired difference has zero-width CI");

  // Paired CI rejects mismatched lengths.
  threw = false;
  try {
    (void)paired_difference_ci_95({1, 2}, {1});
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  check(threw, "paired CI requires equal-length series");

  if (failures) {
    std::fprintf(stderr, "STAGE6 STATISTICS TESTS FAILED: %d\n", failures);
    return 1;
  }
  std::puts("STAGE6 STATISTICS TESTS PASSED");
  return 0;
}
