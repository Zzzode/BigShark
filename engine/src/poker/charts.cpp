#include <bs/charts.hpp>
#include <sstream>

namespace bs {

static void expand(const std::string& tok, Range169& out) {
  if (tok.size() < 2)
    return;
  char h = tok[0], l = tok[1];
  char suf = tok.size() > 2 ? tok[2] : 0;
  bool plus = tok.size() > 2 && tok.back() == '+';
  bool pair = h == l;
  const std::string R = RANKS;
  auto idx = [&](char c) { return (int)R.find(c); };

  if (pair) {
    if (!plus) {
      out.insert(tok.substr(0, 2));
      return;
    }
    for (int i = idx(l); i < 13; i++) {
      std::string k(2, R[i]);
      out.insert(k);
    }
    return;
  }
  if (idx(h) < idx(l))
    std::swap(h, l);
  if (!plus) {
    out.insert(std::string(1, h) + l + (suf ? std::string(1, suf) : ""));
    return;
  }
  for (int i = idx(l); i < idx(h); i++) {
    std::string k;
    k.push_back(h);
    k.push_back(R[i]);
    if (suf)
      k.push_back(suf);
    out.insert(k);
  }
}

Range169 parseRange(const std::string& spec) {
  Range169 out;
  std::stringstream ss(spec);
  std::string tok;
  while (ss >> tok)
    expand(tok, out);
  return out;
}

const Charts& charts() {
  static Charts c = [] {
    Charts c;
    c.rfi["UTG"] = parseRange("22+ A2s+ KTs+ QJs JTs A9o+ KQo");
    c.rfi["HJ"] = parseRange("22+ A2s+ K9s+ Q9s+ J9s+ T9s 98s 87s 76s A8o+ KJo+");
    c.rfi["CO"] = parseRange("22+ A2s+ K7s+ Q8s+ J8s+ T8s+ 97s+ 87s 76s 65s 54s A7o+ K9o+ QJo");
    c.rfi["BTN"] =
        parseRange("22+ A2s+ K5s+ Q7s+ J7s+ T7s+ 97s+ 86s+ 75s+ 65s 54s A2o+ K7o+ Q9o+ J9o+ T9o");
    c.rfi["SB"] = parseRange("22+ A2s+ K8s+ Q8s+ J8s+ T8s+ 98s 87s 76s A4o+ K9o+ QJo");

    c.vs["EP"] = {parseRange("TT+ AQs+ AKo"), parseRange("A2s A3s A4s A5s KQs QJs JTs"),
                  parseRange("22 33 44 55 66 77 88 99 AJs KQs AQo")};
    c.vs["HJ"] = {parseRange("99+ AQs+ AQo+"), parseRange("A2s A3s A4s A5s KQs QJs JTs T9s"),
                  parseRange("22 33 44 55 66 77 88 AJs ATs KQs QJs AQo KQo")};
    c.vs["CO"] = {parseRange("88+ AJs+ AQo+"),
                  parseRange("A2s A3s A4s A5s KJs KQs QTs JTs T9s 98s KQo"),
                  parseRange("22 33 44 55 66 77 A9s+ ATs KJs KTs QJs JTs AJo KQo")};
    c.vs["BTN"] = {parseRange("66+ AQs+ AQo+"),
                   parseRange("A2s+ K7s+ Q9s+ J9s+ T9s 98s 87s K9o+ QJo"),
                   parseRange("22 33 44 55 A8s+ K9s+ Q9s+ J9s+ T9s A9o+ KTo+ QTo+ JTo")};
    c.vs["SB"] = {parseRange("55+ AQs+ AQo+"),
                  parseRange("A2s+ K5s+ Q8s+ J8s+ T8s+ 98s 87s 76s A2o+ K8o+ Q9o+"),
                  parseRange("22 33 44 A7s+ K8s+ Q9s+ J9s+ T9s 98s A8o+ KTo+ QTo")};

    c.fourValue = parseRange("AA KK QQ AKs AKo");
    c.fourBluff = parseRange("A5s A4s");
    c.vs3Call = parseRange("22 33 44 55 66 77 88 99 TT JJ AQs AQo AJs KQs QJs JTs");
    c.vs4Continue = parseRange("AA KK AKs AKo");
    return c;
  }();
  return c;
}

const Range169& rfiRange(const std::string& position) {
  static const Range169 empty;
  const auto& m = charts().rfi;
  auto it = m.find(position);
  return it != m.end() ? it->second : empty;
}
const VsOpen& vsOpenRange(const std::string& bucket) {
  static const VsOpen empty{Range169{}, Range169{}, Range169{}};
  const auto& m = charts().vs;
  auto it = m.find(bucket);
  return it != m.end() ? it->second : (m.count("HJ") ? m.at("HJ") : empty);
}

}  // namespace bs
