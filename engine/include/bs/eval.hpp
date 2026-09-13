// eval.hpp — fast 5–7 card evaluator, bitmask based.
// Card id = rank*4 + suit, rank 0..12 (2..A), suit 0..3 (s,h,d,c).
// Score packs category (9 best) in high bits followed by rank nibbles,
// so comparing scores is comparing hands.
#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace bs {

inline int cardId(char r, char s) {
  static const char* R = "23456789TJQKA";
  int rank = 0;
  for (int i = 0; i < 13; i++)
    if (R[i] == r) {
      rank = i;
      break;
    }
  int suit = s == 's' ? 0 : s == 'h' ? 1 : s == 'd' ? 2 : 3;
  return rank * 4 + suit;
}

inline int cardId(const std::string& two) {
  return cardId(two[0], two[1]);
}

struct HandVal {
  int cat;  // 1 high card .. 9 straight flush
  uint32_t score;
};

inline uint32_t packScore(int cat, const std::array<int, 5>& tie) {
  uint32_t s = (uint32_t)cat << 20;
  for (int i = 0; i < 5; i++)
    s |= (uint32_t)(tie[i] & 0xF) << (16 - 4 * i);
  return s;
}

// highest 5-card straight rank present in mask (bit i = rank i, A=12); 0 if none
inline int straightHigh(uint16_t mask) {
  for (int hi = 12; hi >= 4; hi--) {
    bool ok = true;
    for (int k = 0; k < 5; k++)
      if (!(mask & (1 << (hi - k)))) {
        ok = false;
        break;
      }
    if (ok)
      return hi;
  }
  // wheel A-5: A(12),2(0),3(1),4(2),5(3)
  if ((mask & (1 << 12)) && (mask & 0b1111) == 0b1111)
    return 3;  // five-high, high rank index = 3 ('5')
  return 0;
}

inline HandVal evaluate(const int* cards, int n) {
  uint8_t rankCount[13] = {};
  uint16_t suitMask[4] = {};
  for (int i = 0; i < n; i++) {
    int rk = cards[i] >> 2;
    int st = cards[i] & 3;
    rankCount[rk]++;
    suitMask[st] |= (uint16_t)(1 << rk);
  }

  // collect ranks by multiplicity
  int quads = -1, tripsHi = -1, tripsLo = -1;
  int pairHi = -1, pairLo = -1;
  int trips[4] = {}, pairs[6] = {};
  int nt = 0, np = 0;
  for (int r = 12; r >= 0; r--) {
    if (rankCount[r] == 4)
      quads = r;
    else if (rankCount[r] == 3)
      trips[nt++] = r;
    else if (rankCount[r] == 2)
      pairs[np++] = r;
  }
  if (nt >= 1)
    tripsHi = trips[0];
  if (nt >= 2)
    tripsLo = trips[1];
  if (np >= 1)
    pairHi = pairs[0];
  if (np >= 2)
    pairLo = pairs[1];

  auto kickers = [&](int excludeRanks[], int nExclude, int count) {
    std::array<int, 5> t{0, 0, 0, 0, 0};
    int k = 0;
    for (int r = 12; r >= 0 && k < count; r--) {
      bool ex = false;
      for (int e = 0; e < nExclude; e++)
        if (excludeRanks[e] == r)
          ex = true;
      int copies = ex ? 0 : rankCount[r];
      while (copies-- > 0 && k < count)
        t[k++] = r + 2;
    }
    return t;
  };

  // straight flush
  for (int s = 0; s < 4; s++) {
    if (__builtin_popcount(suitMask[s]) >= 5) {
      int hi = straightHigh(suitMask[s]);
      if (hi)
        return {9, packScore(9, {hi + 2, 0, 0, 0, 0})};
    }
  }
  // four of a kind
  if (quads >= 0) {
    int ex[1] = {quads};
    auto t = kickers(ex, 1, 1);
    return {8, packScore(8, {quads + 2, t[0], 0, 0, 0})};
  }
  // full house: trips + pair (or two trips)
  if (tripsHi >= 0 && (pairHi >= 0 || tripsLo >= 0)) {
    int pr = pairHi >= 0 ? pairHi : tripsLo;
    return {7, packScore(7, {tripsHi + 2, pr + 2, 0, 0, 0})};
  }
  // flush
  for (int s = 0; s < 4; s++) {
    if (__builtin_popcount(suitMask[s]) >= 5) {
      std::array<int, 5> t{0, 0, 0, 0, 0};
      int k = 0;
      for (int r = 12; r >= 0 && k < 5; r--)
        if (suitMask[s] & (1 << r))
          t[k++] = r + 2;
      return {6, packScore(6, t)};
    }
  }
  // straight
  uint16_t allMask = 0;
  for (int s = 0; s < 4; s++)
    allMask |= suitMask[s];
  if (int hi = straightHigh(allMask))
    return {5, packScore(5, {hi + 2, 0, 0, 0, 0})};

  // trips
  if (tripsHi >= 0) {
    int ex[1] = {tripsHi};
    auto t = kickers(ex, 1, 2);
    return {4, packScore(4, {tripsHi + 2, t[0], t[1], 0, 0})};
  }
  // two pair
  if (pairLo >= 0) {
    int ex[2] = {pairHi, pairLo};
    auto t = kickers(ex, 2, 1);
    return {3, packScore(3, {pairHi + 2, pairLo + 2, t[0], 0, 0})};
  }
  // one pair
  if (pairHi >= 0) {
    int ex[1] = {pairHi};
    auto t = kickers(ex, 1, 3);
    return {2, packScore(2, {pairHi + 2, t[0], t[1], t[2], 0})};
  }
  // high card
  auto t = kickers(nullptr, 0, 5);
  return {1, packScore(1, t)};
}

}  // namespace bs
