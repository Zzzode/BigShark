#include "river_state_codec.hpp"

#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "offline_census.hpp"
#include "offline_convergence.hpp"
#include "rfc0050_stage1.hpp"
#include "turn_solver.hpp"

namespace bs::rfc0047 {

namespace {

constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

class Fnv64 {
 public:
  void feed(const void* data, std::size_t bytes) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t k = 0; k < bytes; ++k) {
      state_ ^= static_cast<std::uint64_t>(p[k]);
      state_ *= kFnvPrime;
    }
  }
  std::uint64_t value() const { return state_; }

 private:
  std::uint64_t state_ = kFnvOffsetBasis;
};

class Writer {
 public:
  void u8(std::uint8_t v) { put(&v, sizeof(v)); }
  void u32(std::uint32_t v) { put(&v, sizeof(v)); }
  void u64(std::uint64_t v) { put(&v, sizeof(v)); }
  void i32(std::int32_t v) { put(&v, sizeof(v)); }
  void dbl(double v) {
    std::uint64_t bits{};
    std::memcpy(&bits, &v, sizeof(bits));
    u64(bits);
  }
  const std::string& bytes() const { return buf_; }

 private:
  void put(const void* p, std::size_t n) { buf_.append(static_cast<const char*>(p), n); }
  std::string buf_;
};

class Reader {
 public:
  explicit Reader(const std::string& data)
      : ptr_(data.data()), end_(data.data() + data.size()) {}
  Reader(const char* data, std::size_t size) : ptr_(data), end_(data + size) {}
  std::uint8_t u8() { return take<std::uint8_t>(); }
  std::uint32_t u32() { return take<std::uint32_t>(); }
  std::uint64_t u64() { return take<std::uint64_t>(); }
  std::int32_t i32() { return take<std::int32_t>(); }
  double dbl() {
    const std::uint64_t bits = u64();
    double v{};
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }
  bool eof() const { return ptr_ == end_; }
  const char* cursor() const { return ptr_; }
  void advance(std::size_t n) { ptr_ += n; }
  std::size_t remaining() const { return static_cast<std::size_t>(end_ - ptr_); }

 private:
  template <typename T>
  T take() {
    if (static_cast<std::size_t>(end_ - ptr_) < sizeof(T))
      throw std::runtime_error("river checkpoint: truncated read");
    T v{};
    std::memcpy(static_cast<void*>(&v), static_cast<const void*>(ptr_), sizeof(T));
    ptr_ += sizeof(T);
    return v;
  }
  const char* ptr_;
  const char* end_;
};

void write_identity(Writer& w, const bs::poker::GameDef& d) {
  w.u64(static_cast<std::uint64_t>(d.player_count));
  w.u64(static_cast<std::uint64_t>(d.button));
  w.u64(d.big_blind);
  w.u64(d.ante);
  w.u64(d.stacks[0]);
  w.u64(d.stacks[1]);
  w.u64(d.contributions[0]);
  w.u64(d.contributions[1]);
  w.u64(d.pot);
  for (int i = 0; i < 5; ++i)
    w.i32(d.board[i]);
  w.u8(static_cast<std::uint8_t>(d.board_size));
  w.u8(static_cast<std::uint8_t>(d.terminal == bs::poker::TerminalDepth::River ? 1 : 0));
  w.u8(static_cast<std::uint8_t>(d.preflop ? 1 : 0));
}

bs::poker::GameDef read_identity(Reader& r) {
  bs::poker::GameDef d{};
  d.player_count = static_cast<std::size_t>(r.u64());
  d.button = static_cast<std::size_t>(r.u64());
  d.big_blind = r.u64();
  d.ante = r.u64();
  d.stacks[0] = r.u64();
  d.stacks[1] = r.u64();
  d.contributions[0] = r.u64();
  d.contributions[1] = r.u64();
  d.pot = r.u64();
  for (int i = 0; i < 5; ++i)
    d.board[i] = r.i32();
  d.board_size = r.u8();
  // TerminalDepth only distinguishes River and Flop; a river checkpoint must
  // carry the River flag, anything else is an identity failure for this codec.
  if (r.u8() != 1)
    throw std::runtime_error("river checkpoint: non-river terminal depth");
  d.terminal = bs::poker::TerminalDepth::River;
  d.preflop = r.u8() == 1;
  return d;
}

}  // namespace

std::uint64_t river_checkpoint_payload_bytes(const RiverResumableState& state) {
  std::uint64_t bytes = 0;
  for (std::size_t n = 0; n < state.tree_size; ++n) {
    bytes += sizeof(std::uint64_t);  // slot count
    bytes += state.regret[n].size() * sizeof(double);
    bytes += state.average_sum[n].size() * sizeof(double);
  }
  return bytes;
}

void save_river_checkpoint(const std::filesystem::path& path, const RiverResumableState& state,
                           bool rm_plus, bool linear_averaging, bool dcfr) {
  const std::string blob = serialize_river_checkpoint(state, rm_plus, linear_averaging, dcfr);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    throw std::runtime_error("river checkpoint: cannot open output file");
  out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
  if (!out)
    throw std::runtime_error("river checkpoint: write failed");
  out.close();
}

std::string serialize_river_checkpoint(const RiverResumableState& state, bool rm_plus,
                                       bool linear_averaging, bool dcfr) {
  Writer w;
  w.u64(kRiverCheckpointMagic);
  w.u32(kRiverCheckpointFormatVersion);
  write_identity(w, state.def);
  w.u64(state.action_id_digest);
  w.u64(state.tree_size);
  std::uint64_t action_nodes = 0;
  for (const auto& row : state.regret)
    if (!row.empty())
      ++action_nodes;
  w.u64(action_nodes);
  w.u64(state.completed_iterations);
  for (std::uint64_t rung : kRfc0050Stage1Rungs)
    w.u64(rung);
  w.dbl(kRfc0049GateBb);
  w.u8(rm_plus ? 1 : 0);
  w.u8(linear_averaging ? 1 : 0);
  w.u8(dcfr ? 1 : 0);
  const std::uint64_t payload_bytes = river_checkpoint_payload_bytes(state);
  w.u64(payload_bytes);

  Fnv64 hasher;
  hasher.feed(w.bytes().data(), w.bytes().size());
  for (std::size_t n = 0; n < state.tree_size; ++n) {
    const std::uint64_t slots = state.regret[n].size();
    hasher.feed(&slots, sizeof(slots));
    if (slots != 0) {
      hasher.feed(state.regret[n].data(), slots * sizeof(double));
      hasher.feed(state.average_sum[n].data(), slots * sizeof(double));
    }
  }
  w.u64(hasher.value());

  // Payload follows the checksum; the checksum already covers these bytes.
  // One pre-sized buffer and a single write avoid the per-row append churn
  // and a second write syscall on an ~83 MB checkpoint.
  std::string payload;
  payload.resize(static_cast<std::size_t>(payload_bytes));
  char* dst = payload.data();
  for (std::size_t n = 0; n < state.tree_size; ++n) {
    const std::uint64_t slots = state.regret[n].size();
    std::memcpy(dst, &slots, sizeof(slots));
    dst += sizeof(slots);
    if (slots != 0) {
      const std::size_t table_bytes = slots * sizeof(double);
      std::memcpy(dst, state.regret[n].data(), table_bytes);
      dst += table_bytes;
      std::memcpy(dst, state.average_sum[n].data(), table_bytes);
      dst += table_bytes;
    }
  }

  std::string out_blob;
  out_blob.reserve(w.bytes().size() + static_cast<std::size_t>(payload_bytes));
  out_blob.append(w.bytes());
  out_blob.append(payload);
  return out_blob;
}

RiverResumableState load_river_checkpoint(const std::filesystem::path& path,
                                          const bs::poker::GameDef& expected_def,
                                          std::uint64_t expected_action_digest) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("river checkpoint: cannot open input file");
  std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  Reader r(raw);
  const std::uint64_t magic = r.u64();
  if (magic != kRiverCheckpointMagic)
    throw std::runtime_error("river checkpoint: bad magic");
  const std::uint32_t version = r.u32();
  if (version != kRiverCheckpointFormatVersion)
    throw std::runtime_error("river checkpoint: unsupported format version");
  const bs::poker::GameDef stored_def = read_identity(r);
  if (!bs::poker::same_game_def(stored_def, expected_def))
    throw std::runtime_error("river checkpoint: GameDef identity mismatch");
  const std::uint64_t action_digest = r.u64();
  if (action_digest != expected_action_digest)
    throw std::runtime_error("river checkpoint: action digest mismatch");
  const std::uint64_t tree_size = r.u64();
  const std::uint64_t action_nodes = r.u64();
  const std::uint64_t completed = r.u64();
  for (std::uint64_t expected_rung : kRfc0050Stage1Rungs) {
    if (r.u64() != expected_rung)
      throw std::runtime_error("river checkpoint: frozen rung ladder mismatch");
  }
  const double gate = r.dbl();
  if (gate != kRfc0049GateBb)
    throw std::runtime_error("river checkpoint: frozen 0.002 BB gate mismatch");
  const bool rm_plus = r.u8() == 1;
  const bool linear_averaging = r.u8() == 1;
  const bool dcfr = r.u8() == 1;
  if (!rm_plus || !linear_averaging || dcfr)
    throw std::runtime_error("river checkpoint: solver-mode constants mismatch");
  const std::uint64_t payload_bytes = r.u64();
  const std::uint64_t stored_checksum = r.u64();

  // The header prefix excludes the trailing checksum field; the checksum was
  // computed over that prefix and the entire payload at write time.
  const std::size_t header_end = raw.size() - static_cast<std::size_t>(payload_bytes);
  if (raw.size() < header_end || header_end < sizeof(std::uint64_t))
    throw std::runtime_error("river checkpoint: declared payload exceeds file");
  Fnv64 verify;
  verify.feed(raw.data(), header_end - sizeof(std::uint64_t));
  verify.feed(raw.data() + header_end, static_cast<std::size_t>(payload_bytes));
  if (verify.value() != stored_checksum)
    throw std::runtime_error("river checkpoint: checksum mismatch");

  // Rebuild the exact cap-0 tree and cross-check every node width.
  const bs::abstraction::ActionAbstraction exact = bs::abstraction::ActionAbstraction::identity(0);
  const bs::tree::AbstractTree tree(expected_def, exact, offline_census_limits());
  if (tree.size() != tree_size)
    throw std::runtime_error("river checkpoint: rebuilt tree size mismatch");

  RiverResumableState state;
  state.def = expected_def;
  state.action_id_digest = expected_action_digest;
  state.tree_size = tree_size;
  state.completed_iterations = completed;
  state.regret.resize(tree_size);
  state.average_sum.resize(tree_size);

  Reader payload_reader(raw.data() + header_end,
                        static_cast<std::size_t>(payload_bytes));
  std::uint64_t seen_action_nodes = 0;
  for (std::size_t n = 0; n < tree_size; ++n) {
    const std::uint64_t slots = payload_reader.u64();
    const bs::tree::TreeNode& node = tree.node(n);
    if (node.is_action()) {
      const std::uint64_t expected_slots =
          static_cast<std::uint64_t>(kRiverC) * node.actions.size();
      if (slots != expected_slots)
        throw std::runtime_error("river checkpoint: action-node width mismatch");
      ++seen_action_nodes;
      state.regret[n].resize(static_cast<std::size_t>(slots));
      state.average_sum[n].resize(static_cast<std::size_t>(slots));
      for (std::uint64_t k = 0; k < slots; ++k)
        state.regret[n][k] = payload_reader.dbl();
      for (std::uint64_t k = 0; k < slots; ++k)
        state.average_sum[n][k] = payload_reader.dbl();
    } else if (slots != 0) {
      throw std::runtime_error("river checkpoint: non-action node carries slots");
    }
  }
  if (!payload_reader.eof())
    throw std::runtime_error("river checkpoint: trailing payload bytes");
  if (seen_action_nodes != action_nodes)
    throw std::runtime_error("river checkpoint: action-node count mismatch");
  return state;
}

}  // namespace bs::rfc0047
