// RFC 0047 Stage-1 native test: BSNCV01 oracle codec round-trip, integrity,
// and fail-closed behavior; pins the embedded SHA-256 on the FIPS "abc"
// vector.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>

#include "../benchmarks/rfc0047/cv_oracle.hpp"

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

using namespace bs::rfc0047;

bool test_sha256_abc() {
  const std::string s = "abc";
  const std::string hex = sha256_hex(
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
  CHECK(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  return true;
}

OracleRow row(std::uint32_t leaf, std::uint16_t combo, double cv, double err, std::uint8_t seat = 0,
              std::uint8_t shape = 7) {
  OracleRow r;
  r.key.shape_id = shape;
  // A canonical-ish five-card board (ids need only be five distinct cards).
  r.key.board5 = {0, 1, 2, 3, 4};
  r.key.frontier_leaf = leaf;
  r.key.seat = seat;
  r.key.combo_dense = combo;  // board 0..4 leaves live cards starting at 5
  r.cv_chips = cv;
  r.certified_error_chips = err;
  r.root_pot_chips = 8;
  return r;
}

bool throws_like(const std::function<void()>& fn) {
  try {
    fn();
  } catch (const std::exception&) {
    return true;
  }
  return false;
}

bool test_roundtrip_lookup() {
  const auto path = std::filesystem::temp_directory_path() / "rfc0047_oracle_test.bsncv";
  OracleWriter w(7, 8);
  w.add(row(0, 0, 1.25, 0.0003));
  w.add(row(0, 1, -2.5, 0.0004, 1));
  w.add(row(3, 1080, 0.0, 0.0));  // max legal combo
  w.write(path);

  const OracleReader r = OracleReader::open(path);
  CHECK(r.shape_id() == 7);
  CHECK(r.default_root_pot() == 8);
  CHECK(r.size() == 3);
  const OracleRow* a = r.lookup(row(0, 0, 0, 0).key);
  CHECK(a != nullptr && a->cv_chips == 1.25 && a->certified_error_chips == 0.0003);
  const OracleRow* b = r.lookup(row(0, 1, 0, 0, 1).key);
  CHECK(b != nullptr && b->key.seat == 1 && b->cv_chips == -2.5);
  CHECK(r.lookup(row(0, 2, 0, 0).key) == nullptr);  // absent -> nullopt
  std::filesystem::remove(path);
  return true;
}

bool test_tamper_and_malformed() {
  const auto path = std::filesystem::temp_directory_path() / "rfc0047_oracle_bad.bsncv";
  {
    OracleWriter w(7, 8);
    w.add(row(0, 0, 1.0, 0.01));
    w.write(path);
  }
  // Flip one body byte -> digest mismatch.
  {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    char x = 0;
    f.seekg(40);
    f.read(&x, 1);
    f.seekp(40);
    char y = static_cast<char>(static_cast<unsigned char>(x) ^ 0xFF);
    f.write(&y, 1);
  }
  CHECK(throws_like([&] { (void)OracleReader::open(path); }));
  // Truncate -> fail.
  {
    std::filesystem::resize_file(path, 40);
    CHECK(throws_like([&] { (void)OracleReader::open(path); }));
  }
  std::filesystem::remove(path);

  // Bad fields at write time.
  CHECK(throws_like([&] {
    OracleWriter w(7, 8);
    w.add(row(0, 1081, 1.0, 0.01));  // combo out of range
  }));
  CHECK(throws_like([&] {
    OracleWriter w(7, 8);
    OracleRow r = row(0, 0, 1.0, -0.01);  // negative error
    w.add(r);
  }));
  return true;
}

bool test_duplicate_rejected() {
  OracleWriter w(7, 8);
  w.add(row(1, 5, 1.0, 0.0));
  bool threw = false;
  try {
    w.add(row(1, 5, 2.0, 0.0));  // same key
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
  // Zero pot rejected at construction.
  bool threw2 = false;
  try {
    OracleWriter bad(7, 0);
    (void)bad;
  } catch (const std::exception&) {
    threw2 = true;
  }
  CHECK(threw2);
  return true;
}

int main() {
  if (!test_sha256_abc() || !test_roundtrip_lookup() || !test_tamper_and_malformed() ||
      !test_duplicate_rejected())
    return 1;
  std::printf("rfc0047_oracle_codec: all checks passed\n");
  return 0;
}
