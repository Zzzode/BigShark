// stage6/statistics.hpp — R8/R9 paired-sample Student-t statistics.
//
// The estimator produces, per seed s, one vector g_s = (g_{1,s}..g_{N,s}) of
// per-seat unilateral gains. The NashConv scalar replicate is the SUM
// Y_s = sum_i g_{i,s}; cross-seat gains on one deal are correlated, so the
// CI must be formed on that scalar, never by summing per-seat interval
// half-widths. Profile comparisons use the paired difference of two scalar
// series measured on identical seeds. This module is pure math; it has no
// poker dependency so it can be unit tested in isolation.
#pragma once

#include <cstddef>
#include <vector>

namespace bs::stage6 {

struct SampleMoments {
  std::size_t n = 0;
  double mean = 0.0;
  double sample_variance = 0.0;  // unbiased 1/(n-1) estimator
  double standard_error = 0.0;
};

SampleMoments sample_moments(const std::vector<double>& values);

// Two-sided Student-t critical value for the given degrees of freedom at the
// declared 95% level. Small-df values are tabulated (RFC 0006's pinned seeds
// give small n before the pilot expands it); for larger df a normal tail
// approximation is used. Throws for df == 0.
double student_t_critical_95(std::size_t degrees_of_freedom);

struct ConfidenceInterval {
  double mean = 0.0;
  double half_width = 0.0;
  double lower = 0.0;
  double upper = 0.0;
  std::size_t df = 0;
};

// Two-sided 95% CI for the mean of one scalar replicate series.
ConfidenceInterval confidence_interval_95(const std::vector<double>& values);

// Paired 95% CI for the mean difference of two series measured on identical
// seeds (a[s] - b[s]). The series must be the same length; the test uses the
// variance of the within-pair differences.
ConfidenceInterval paired_difference_ci_95(const std::vector<double>& a,
                                           const std::vector<double>& b);

// Sums the per-seat vector for one replicate: Y_s = sum_i gains[s][i]. Used to
// turn the estimator's vector output into the scalar the CI is computed on.
double sum_seats(const std::vector<double>& per_seat_gains);

}  // namespace bs::stage6
