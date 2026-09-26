#include <bs/stage6/statistics.hpp>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace bs::stage6 {

SampleMoments sample_moments(const std::vector<double>& values) {
  SampleMoments m;
  m.n = values.size();
  if (m.n == 0)
    return m;
  double sum = 0.0;
  for (double v : values)
    sum += v;
  m.mean = sum / static_cast<double>(m.n);
  if (m.n >= 2) {
    double acc = 0.0;
    for (double v : values) {
      const double d = v - m.mean;
      acc += d * d;
    }
    m.sample_variance = acc / static_cast<double>(m.n - 1);
    m.standard_error = std::sqrt(m.sample_variance / static_cast<double>(m.n));
  }
  return m;
}

double student_t_critical_95(std::size_t df) {
  if (df == 0)
    throw std::invalid_argument("t critical value requires degrees of freedom >= 1");
  // Two-sided 0.95 -> upper tail 0.025, tabulated for the small samples the
  // pilot/confirmatory lists produce before expansion.
  static constexpr std::size_t kTableDf[] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
                                             11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
                                             21, 22, 23, 24, 25, 26, 27, 28, 29, 30};
  static constexpr double kTableT[] = {
      12.7062, 4.3027, 3.1824, 2.7764, 2.5706, 2.4469, 2.3646, 2.3060, 2.2622, 2.2281,
      2.2010,  2.1788, 2.1604, 2.1448, 2.1314, 2.1199, 2.1098, 2.1009, 2.0930, 2.0860,
      2.0796,  2.0739, 2.0687, 2.0639, 2.0595, 2.0555, 2.0518, 2.0484, 2.0452, 2.0423};
  constexpr std::size_t kTableSize = sizeof(kTableDf) / sizeof(kTableDf[0]);
  if (df <= kTableDf[kTableSize - 1]) {
    for (std::size_t i = 0; i < kTableSize; ++i)
      if (kTableDf[i] == df)
        return kTableT[i];
  }
  // Above df 30 the Cornish-Fisher expansion on the standard normal 0.975
  // quantile (1.959964) is accurate to a few 1e-4, ample for a CI endpoint.
  const double z = 1.959963984540054;
  const double g1 = (z * z * z + z) / 4.0;
  const double g2 = (5 * std::pow(z, 5) + 16 * std::pow(z, 3) + 3 * z) / 96.0;
  const double g3 =
      (3 * std::pow(z, 7) + 19 * std::pow(z, 5) + 17 * std::pow(z, 3) - 15 * z) / 384.0;
  const double d = static_cast<double>(df);
  return z + g1 / d + g2 / (d * d) + g3 / (d * d * d);
}

ConfidenceInterval confidence_interval_95(const std::vector<double>& values) {
  ConfidenceInterval ci;
  const SampleMoments m = sample_moments(values);
  if (m.n < 2)
    throw std::invalid_argument("a confidence interval needs at least two replicates");
  ci.df = m.n - 1;
  ci.mean = m.mean;
  ci.half_width = student_t_critical_95(ci.df) * m.standard_error;
  ci.lower = ci.mean - ci.half_width;
  ci.upper = ci.mean + ci.half_width;
  return ci;
}

ConfidenceInterval paired_difference_ci_95(const std::vector<double>& a,
                                           const std::vector<double>& b) {
  if (a.size() != b.size())
    throw std::invalid_argument("paired series must have the same length");
  std::vector<double> differences;
  differences.reserve(a.size());
  for (std::size_t i = 0; i < a.size(); ++i)
    differences.push_back(a[i] - b[i]);
  return confidence_interval_95(differences);
}

double sum_seats(const std::vector<double>& per_seat_gains) {
  double total = 0.0;
  for (double g : per_seat_gains)
    total += g;
  return total;
}

}  // namespace bs::stage6
