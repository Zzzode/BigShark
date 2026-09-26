#include <bs/stage6/public_path.hpp>
#include <cstdint>
#include <sstream>

namespace bs::stage6 {

std::uint64_t PublicPath::hash() const noexcept {
  // FNV-1a over the LITTLE-ENDIAN BYTES of every uint16 token, in order. This
  // is the same canonical 64-bit hash used for artifact rows and the geometry
  // matrix; an earlier boost::hash_combine idiom over the uint16 VALUES was
  // lossy on deep small-alphabet paths and collides two distinct action/card
  // sequences at live>=4 (regression: the streamed multiway table merged a
  // check-to node with a facing-a-bet node). Length must enter the hash too,
  // because a trailing zero token must not be a prefix of the same digest.
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (std::uint16_t token : tokens_) {
    hash ^= static_cast<std::uint64_t>(token & 0xffu);
    hash *= 0x100000001b3ULL;
    hash ^= static_cast<std::uint64_t>((token >> 8) & 0xffu);
    hash *= 0x100000001b3ULL;
  }
  hash ^= static_cast<std::uint64_t>(tokens_.size());
  hash *= 0x100000001b3ULL;
  return hash;
}

std::string PublicPath::to_string() const {
  std::ostringstream os;
  for (std::uint16_t token : tokens_)
    os << token << ';';
  return os.str();
}

}  // namespace bs::stage6
