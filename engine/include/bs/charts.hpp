// charts.hpp — 169-hand keys, Chen ordering percentile, 6-max 100bb
// solver-approximation preflop charts (RFI / vs open / vs 3bet / vs 4bet).
#pragma once
#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bs {

inline const char* RANKS = "23456789TJQKA";

inline std::string key169(int idA, int idB) {
  int ra = idA >> 2, rb = idB >> 2;
  int hi = std::max(ra, rb), lo = std::min(ra, rb);
  std::string k;
  k.push_back(RANKS[hi]);
  k.push_back(RANKS[lo]);
  if (hi != lo)
    k.push_back((idA & 3) == (idB & 3) ? 's' : 'o');
  return k;
}

inline double chenScore(const std::string& k) {
  int hi = std::string(RANKS).find(k[0]), lo = std::string(RANKS).find(k[1]);
  bool pair = k[0] == k[1];
  bool suited = k.size() > 2 && k[2] == 's';
  static const double BV[13] = {1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5, 5, 6, 7, 8, 10};
  if (pair)
    return std::max(5.0, (double)(hi + 2) * 2.0);
  double v = BV[hi];
  if (suited)
    v += 2;
  int gap = hi - lo - 1;
  if (gap == 1)
    v -= 1;
  else if (gap == 2)
    v -= 2;
  else if (gap == 3)
    v -= 4;
  else if (gap >= 4)
    v -= 5;
  return v;
}

// percentile map: strongest hand -> ~1.0 (built lazily, cached)
inline const std::unordered_map<std::string, double>& preflopPctTable() {
  static std::unordered_map<std::string, double> table;
  if (table.empty()) {
    std::vector<std::pair<double, std::string>> all;
    for (int hi = 12; hi >= 0; hi--)
      for (int lo = hi; lo >= 0; lo--) {
        std::string base;
        base.push_back(RANKS[hi]);
        base.push_back(RANKS[lo]);
        if (hi == lo)
          all.emplace_back(chenScore(base), base);
        else {
          all.emplace_back(chenScore(base + 's'), base + 's');
          all.emplace_back(chenScore(base + 'o'), base + 'o');
        }
      }
    std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.first > b.first; });
    const double n = (double)all.size();
    for (size_t i = 0; i < all.size(); i++)
      table[all[i].second] = 1.0 - (double)i / n;
  }
  return table;
}
inline double preflopPct(const std::string& k) {
  auto& t = preflopPctTable();
  auto it = t.find(k);
  return it == t.end() ? 0.0 : it->second;
}

using Range169 = std::unordered_set<std::string>;
Range169 parseRange(const std::string& spec);

// ---------- charts ----------
struct VsOpen {
  Range169 value, bluff, call;
};

struct Charts {
  std::unordered_map<std::string, Range169> rfi;  // pos -> open range
  std::unordered_map<std::string, VsOpen> vs;     // opener bucket EP/HJ/CO/BTN/SB
  Range169 fourValue, fourBluff, vs3Call, vs4Continue;
};

const Charts& charts();

// public range accessors for combo-level construction
const Range169& rfiRange(const std::string& position);
const VsOpen& vsOpenRange(const std::string& openerBucket);

}  // namespace bs
