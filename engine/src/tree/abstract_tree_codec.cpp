// ATF binary serialization for AbstractTree.
//
// Saves the tree structure (nodes, children, actions) and terminal payloads
// in a compact binary format, compressed with zstd.
//
// File layout (little-endian x86_64 Linux):
//   ATFZ (zstd-compressed):
//     char     magic[4]        = "ATFZ"
//     uint32   version         = 2
//     uint64   uncompressed_size
//     <zstd compressed payload>
//
//   Payload (decompressed):
//     Header (32 bytes):
//       char     magic[4]        = "ATF1"
//       uint32   version         = 2
//       uint32   node_count
//       uint32   terminal_count
//       uint32   depth
//       uint64   accounted_bytes
//       uint32   reserved        = 0
//     For each node (24-byte fixed header + variable children + actions):
//       uint8    kind            (0=Action, 1=Chance, 2=TerminalFold,
//                                 3=TerminalShowdown, 4=TerminalFrontier,
//                                 5=FlopDeal)
//       uint8    street          (0=Flop, 1=Turn, 2=River, 3=Preflop)
//       uint8    action_count
//       uint8    incoming_action_type (0=Fold, 1=Check, 2=Call, 3=Bet, 4=Raise)
//       uint32   actor           (0xFFFFFFFF for kNoNode)
//       int32    incoming_card   (-1 for kNoCard)
//       uint32   terminal        (0xFFFFFFFF for kNoNode)
//       uint32   child_count
//       uint32   incoming_action_target
//       uint32   children[child_count]
//       For each action:
//         uint8    type
//         uint32   target_total
//     For each terminal:
//       uint8    player_count
//       uint8    live_count
//       uint8    folded          (0 or 1)
//       uint8    board_size
//       For each seat (player_count seats, 32 bytes each):
//         int64    contributed
//         int64    refunded
//         int64    stack
//         uint8    folded
//         uint8    pad[7]
//       For each live seat (live_count entries):
//         uint32   live_order[i]
//       int32    board[5]
//       uint32   chip_utility_count
//       int64    chip_utility[chip_utility_count]

#include <zstd.h>

#include <algorithm>
#include <bs/abstract_tree.hpp>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace bs::tree {

namespace {

constexpr char kMagic[4] = {'A', 'T', 'F', '1'};
constexpr char kMagicCompressed[4] = {'A', 'T', 'F', 'Z'};
constexpr std::uint32_t kVersion = 2;
constexpr std::uint32_t kNoNodeU32 = 0xFFFFFFFFu;
constexpr int kZstdLevel = 3;

void write_u8(std::ostream& out, std::uint8_t v) {
  out.put(static_cast<char>(v));
}

void write_u32(std::ostream& out, std::uint32_t v) {
  out.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

void write_u64(std::ostream& out, std::uint64_t v) {
  out.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

void write_i32(std::ostream& out, std::int32_t v) {
  out.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

void write_i64(std::ostream& out, std::int64_t v) {
  out.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

std::uint32_t read_u32(std::istream& in) {
  std::uint32_t v = 0;
  in.read(reinterpret_cast<char*>(&v), sizeof(v));
  if (!in)
    throw std::runtime_error("abstract_tree_codec: truncated compressed header");
  return v;
}

std::uint64_t read_u64(std::istream& in) {
  std::uint64_t v = 0;
  in.read(reinterpret_cast<char*>(&v), sizeof(v));
  if (!in)
    throw std::runtime_error("abstract_tree_codec: truncated compressed header");
  return v;
}

std::uint8_t action_type_to_byte(poker::ActionType type) {
  switch (type) {
    case poker::ActionType::Fold:
      return 0;
    case poker::ActionType::Check:
      return 1;
    case poker::ActionType::Call:
      return 2;
    case poker::ActionType::Bet:
      return 3;
    case poker::ActionType::Raise:
      return 4;
  }
  throw std::runtime_error("abstract_tree_codec: unknown action type");
}

poker::ActionType byte_to_action_type(std::uint8_t byte) {
  switch (byte) {
    case 0:
      return poker::ActionType::Fold;
    case 1:
      return poker::ActionType::Check;
    case 2:
      return poker::ActionType::Call;
    case 3:
      return poker::ActionType::Bet;
    case 4:
      return poker::ActionType::Raise;
    default:
      throw std::runtime_error("abstract_tree_codec: invalid action type byte " +
                               std::to_string(byte));
  }
}

std::uint8_t node_kind_to_byte(NodeKind kind) {
  switch (kind) {
    case NodeKind::Action:
      return 0;
    case NodeKind::Chance:
      return 1;
    case NodeKind::TerminalFold:
      return 2;
    case NodeKind::TerminalShowdown:
      return 3;
    case NodeKind::TerminalFrontier:
      return 4;
    case NodeKind::FlopDeal:
      return 5;
  }
  throw std::runtime_error("abstract_tree_codec: unknown node kind");
}

NodeKind byte_to_node_kind(std::uint8_t byte) {
  switch (byte) {
    case 0:
      return NodeKind::Action;
    case 1:
      return NodeKind::Chance;
    case 2:
      return NodeKind::TerminalFold;
    case 3:
      return NodeKind::TerminalShowdown;
    case 4:
      return NodeKind::TerminalFrontier;
    case 5:
      return NodeKind::FlopDeal;
    default:
      throw std::runtime_error("abstract_tree_codec: invalid node kind byte " +
                               std::to_string(byte));
  }
}

void check_write(const std::ostream& out, const std::string& what) {
  if (!out)
    throw std::runtime_error("abstract_tree_codec: " + what + " failed");
}

// A fast cursor over a contiguous byte buffer. Replaces std::istringstream
// for tree deserialization: memcpy reads at >10 GB/s vs ~82 MB/s for
// istringstream virtual dispatch. Throws std::runtime_error on end-of-data.
class Cursor {
 public:
  Cursor(const char* data, std::size_t size, std::string context = "")
      : data_(data), size_(size), context_(std::move(context)) {}

  template <typename T>
  T read() {
    if (sizeof(T) > size_ - pos_)
      throw std::runtime_error("abstract_tree_codec: unexpected end of data" +
                               (context_.empty() ? "" : " in " + context_));
    T v;
    std::memcpy(&v, data_ + pos_, sizeof(T));
    pos_ += sizeof(T);
    return v;
  }

  void read_bytes(char* dst, std::size_t n) {
    if (n > size_ - pos_)
      throw std::runtime_error("abstract_tree_codec: unexpected end of data" +
                               (context_.empty() ? "" : " in " + context_));
    std::memcpy(dst, data_ + pos_, n);
    pos_ += n;
  }

  void skip(std::size_t n) {
    if (n > size_ - pos_)
      throw std::runtime_error("abstract_tree_codec: unexpected end of data" +
                               (context_.empty() ? "" : " in " + context_));
    pos_ += n;
  }

  std::size_t pos() const noexcept { return pos_; }
  std::size_t remaining() const noexcept { return size_ - pos_; }

 private:
  const char* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
  std::string context_;
};

// Charge actual retained vector slabs before allocation. Serialized accounting
// is advisory and cannot establish a safe allocation bound for a loaded tree.
class LoadBudget {
 public:
  explicit LoadBudget(std::size_t limit) : limit_(limit) {}

  void charge(std::size_t count, std::size_t width) {
    if (count > (limit_ - bytes_) / width)
      throw tree_resource_exhausted("abstract tree load exceeded the byte cap");
    bytes_ += count * width;
  }

  std::size_t bytes() const { return bytes_; }

 private:
  std::size_t limit_;
  std::size_t bytes_ = 0;
};

void require_valid(bool valid, const char* message) {
  if (!valid)
    throw std::runtime_error(std::string("abstract_tree_codec: ") + message);
}

bool read_bool(Cursor& cursor) {
  const auto value = cursor.read<std::uint8_t>();
  require_valid(value <= 1, "invalid boolean flag");
  return value != 0;
}

poker::Chips read_chips(Cursor& cursor) {
  const auto value = cursor.read<std::int64_t>();
  require_valid(value >= 0 && static_cast<poker::Chips>(value) <= poker::kMaxHeadsUpChips,
                "terminal chips exceed numeric bound");
  return static_cast<poker::Chips>(value);
}

bool aggressive(Action action) {
  return action.type == poker::ActionType::Bet || action.type == poker::ActionType::Raise;
}

// Serialize one terminal payload to the output stream.
void serialize_terminal(const TerminalPayload& term, std::ostream& out) {
  write_u8(out, static_cast<std::uint8_t>(term.player_count));
  write_u8(out, static_cast<std::uint8_t>(term.live_count));
  write_u8(out, term.folded ? 1 : 0);
  write_u8(out, term.board_size);

  // Per-seat data: 32 bytes each (24 bytes data + 7 bytes pad + 1 byte folded)
  for (std::size_t s = 0; s < term.player_count; ++s) {
    write_i64(out, term.seats[s].contributed);
    write_i64(out, term.seats[s].refunded);
    write_i64(out, term.seats[s].stack);
    write_u8(out, term.seats[s].folded ? 1 : 0);
    static constexpr char kPad[7] = {};
    out.write(kPad, 7);
  }

  // live_order
  for (std::size_t i = 0; i < term.live_count; ++i)
    write_u32(out, static_cast<std::uint32_t>(term.live_order[i]));

  // board (always 5 entries)
  for (int i = 0; i < 5; ++i)
    write_i32(out, term.board[i]);

  // chip_utility (only populated for fold leaves)
  write_u32(out, static_cast<std::uint32_t>(term.chip_utility.size()));
  for (std::int64_t v : term.chip_utility)
    write_i64(out, v);
}

// Deserialize one terminal payload from a cursor.
TerminalPayload deserialize_terminal(Cursor& cursor, const GameState& root, LoadBudget& budget) {
  TerminalPayload term;
  term.player_count = cursor.read<std::uint8_t>();
  term.live_count = cursor.read<std::uint8_t>();
  term.folded = read_bool(cursor);
  term.board_size = cursor.read<std::uint8_t>();
  require_valid(term.player_count == root.player_count() && term.player_count <= term.seats.size(),
                "invalid terminal player count");
  require_valid(term.live_count > 0 && term.live_count <= term.player_count,
                "invalid terminal live count");
  require_valid(term.board_size <= term.board.size(), "invalid terminal board size");
  require_valid(term.folded ? term.live_count == 1 : term.live_count >= 2,
                "terminal fold flag disagrees with live count");

  // Per-seat data
  std::size_t nonfolded = 0;
  poker::Chips pot = 0;
  for (std::size_t s = 0; s < term.player_count; ++s) {
    term.seats[s].contributed = read_chips(cursor);
    term.seats[s].refunded = read_chips(cursor);
    term.seats[s].stack = read_chips(cursor);
    term.seats[s].folded = read_bool(cursor);
    require_valid(term.seats[s].refunded <= term.seats[s].contributed,
                  "terminal refund exceeds contribution");
    const auto net = term.seats[s].contributed - term.seats[s].refunded;
    const auto& initial = root.players()[s];
    const auto initial_total = initial.stack + initial.contributed - initial.refunded;
    require_valid(net <= initial_total && term.seats[s].stack == initial_total - net,
                  "terminal ledger violates seat chip conservation");
    require_valid(!initial.folded || term.seats[s].folded, "terminal revives a folded seat");
    require_valid(net <= poker::kMaxHeadsUpChips - pot, "terminal pot exceeds numeric bound");
    pot += net;
    nonfolded += !term.seats[s].folded;
    cursor.skip(7);  // padding
  }
  require_valid(nonfolded == term.live_count, "terminal live seats disagree with ledger");

  // live_order
  for (std::size_t i = 0; i < term.live_count; ++i) {
    term.live_order[i] = cursor.read<std::uint32_t>();
    require_valid(term.live_order[i] < term.player_count &&
                      !term.seats[term.live_order[i]].folded &&
                      (i == 0 || term.live_order[i - 1] < term.live_order[i]),
                  "invalid terminal live seat order");
  }

  // board
  std::uint64_t cards = 0;
  for (int i = 0; i < 5; ++i) {
    term.board[i] = cursor.read<std::int32_t>();
    if (i < term.board_size) {
      require_valid(term.board[i] >= 0 && term.board[i] < 52, "invalid terminal board card");
      const auto bit = std::uint64_t{1} << term.board[i];
      require_valid((cards & bit) == 0, "duplicate terminal board card");
      cards |= bit;
    }
  }

  // chip_utility
  const std::uint32_t cu_count = cursor.read<std::uint32_t>();
  require_valid(cu_count == (term.folded ? term.player_count : 0),
                "invalid terminal utility count");
  require_valid(cu_count <= cursor.remaining() / sizeof(std::int64_t),
                "truncated terminal utilities");
  budget.charge(cu_count, sizeof(std::int64_t));
  term.chip_utility.resize(cu_count);
  for (std::uint32_t i = 0; i < cu_count; ++i) {
    term.chip_utility[i] = cursor.read<std::int64_t>();
    const auto net = term.seats[i].contributed - term.seats[i].refunded;
    const auto expected = (i == term.live_order[0]) ? static_cast<std::int64_t>(pot - net)
                                                    : -static_cast<std::int64_t>(net);
    require_valid(term.chip_utility[i] == expected, "invalid fold utility");
  }

  return term;
}

// Serialize the tree structure to a stream.
void serialize_payload(const AbstractTree& tree, std::ostream& out) {
  // Header (32 bytes)
  out.write(kMagic, 4);
  write_u32(out, kVersion);
  write_u32(out, static_cast<std::uint32_t>(tree.nodes().size()));
  write_u32(out, static_cast<std::uint32_t>(tree.terminals().size()));
  write_u32(out, static_cast<std::uint32_t>(tree.depth()));
  write_u64(out, static_cast<std::uint64_t>(tree.accounted_bytes()));
  write_u32(out, 0);  // reserved

  // Nodes
  for (const TreeNode& node : tree.nodes()) {
    write_u8(out, node_kind_to_byte(node.kind));
    write_u8(out, static_cast<std::uint8_t>(node.street));
    write_u8(out, static_cast<std::uint8_t>(node.actions.size()));
    write_u8(out, action_type_to_byte(node.incoming_action.type));
    write_u32(out, node.actor == kNoNode ? kNoNodeU32 : static_cast<std::uint32_t>(node.actor));
    write_i32(out, static_cast<std::int32_t>(node.incoming_card));
    write_u32(out,
              node.terminal == kNoNode ? kNoNodeU32 : static_cast<std::uint32_t>(node.terminal));
    write_u32(out, static_cast<std::uint32_t>(node.children.size()));
    if (node.incoming_action.target_total > std::numeric_limits<std::uint32_t>::max())
      throw std::runtime_error("abstract_tree_codec: incoming action target too large for u32");
    write_u32(out, static_cast<std::uint32_t>(node.incoming_action.target_total));

    for (std::size_t child : node.children) {
      if (child > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("abstract_tree_codec: child index too large for u32");
      write_u32(out, static_cast<std::uint32_t>(child));
    }

    for (const Action& action : node.actions) {
      write_u8(out, action_type_to_byte(action.type));
      if (action.target_total > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("abstract_tree_codec: action target too large for u32");
      write_u32(out, static_cast<std::uint32_t>(action.target_total));
    }
  }

  // Terminal payloads
  for (const TerminalPayload& term : tree.terminals())
    serialize_terminal(term, out);

  check_write(out, "write payload");
}

}  // namespace

// The no-build constructor (declared in the header). Used by load() to
// construct a tree without triggering the builder.
AbstractTree::AbstractTree(GameDef def, abstraction::ActionAbstraction action, TreeLimits limits,
                           no_build_tag)
    : def_(std::move(def)), action_(std::move(action)), limits_(limits) {}

void AbstractTree::save(const std::filesystem::path& path) const {
  // Serialize the payload to an in-memory buffer.
  std::ostringstream oss(std::ios::binary);
  serialize_payload(*this, oss);
  const std::string raw = oss.str();

  // Compress with zstd.
  const std::size_t compressed_bound = ZSTD_compressBound(raw.size());
  std::vector<char> compressed(compressed_bound);
  const std::size_t compressed_size =
      ZSTD_compress(compressed.data(), compressed.size(), raw.data(), raw.size(), kZstdLevel);
  if (ZSTD_isError(compressed_size))
    throw std::runtime_error("abstract_tree_codec: zstd compress failed: " +
                             std::string(ZSTD_getErrorName(compressed_size)));
  compressed.resize(compressed_size);

  // Atomic write: temp file + rename so a crash mid-write does not leave a
  // corrupt file.
  const std::filesystem::path tmp = std::filesystem::path(path.string() + ".tmp");

  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  if (!out)
    throw std::runtime_error("abstract_tree_codec: cannot open " + tmp.string() + " for write");

  // ATFZ header.
  out.write(kMagicCompressed, 4);
  write_u32(out, kVersion);
  write_u64(out, static_cast<std::uint64_t>(raw.size()));
  out.write(compressed.data(), static_cast<std::streamsize>(compressed.size()));

  check_write(out, "write to " + tmp.string());
  out.close();

  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec)
    throw std::runtime_error("abstract_tree_codec: rename failed for " + path.string() + ": " +
                             ec.message());
}

AbstractTree AbstractTree::load(const std::filesystem::path& path, GameDef def,
                                abstraction::ActionAbstraction action, TreeLimits limits) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    throw std::runtime_error("abstract_tree_codec: cannot open " + path.string() + " for read");

  const auto file_end = in.tellg();
  require_valid(file_end >= 0, "cannot determine file size");
  const auto file_size = static_cast<std::uint64_t>(file_end);
  if (file_size > limits.max_bytes ||
      file_size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
    throw tree_resource_exhausted("abstract tree file exceeded the byte cap");
  in.seekg(0);

  // Read magic to detect format.
  char magic[4];
  in.read(magic, 4);
  if (!in)
    throw std::runtime_error("abstract_tree_codec: cannot read magic from " + path.string());

  std::vector<char> payload;

  if (std::memcmp(magic, kMagic, 4) == 0) {
    // ATF1 raw format: read the entire file into a buffer.
    in.seekg(0);
    payload.resize(static_cast<std::size_t>(file_size));
    in.read(payload.data(), static_cast<std::streamsize>(file_size));
    if (!in)
      throw std::runtime_error("abstract_tree_codec: cannot read payload from " + path.string());
  } else if (std::memcmp(magic, kMagicCompressed, 4) == 0) {
    // ATFZ compressed format: read header, decompress.
    require_valid(file_size > 16, "truncated compressed file");
    const std::uint32_t version = read_u32(in);
    if (version != kVersion)
      throw std::runtime_error("abstract_tree_codec: unsupported ATFZ version " +
                               std::to_string(version) + " in " + path.string());

    const std::uint64_t uncompressed_size = read_u64(in);
    require_valid(uncompressed_size >= 32, "invalid uncompressed size");
    if (uncompressed_size > limits.max_bytes)
      throw tree_resource_exhausted("abstract tree payload exceeded the byte cap");

    const std::size_t compressed_size = static_cast<std::size_t>(file_size - 16);
    std::vector<char> compressed(compressed_size);
    in.read(compressed.data(), static_cast<std::streamsize>(compressed_size));
    if (!in)
      throw std::runtime_error("abstract_tree_codec: cannot read compressed data from " +
                               path.string());

    const auto frame_size = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    require_valid(frame_size != ZSTD_CONTENTSIZE_ERROR, "invalid zstd frame");
    require_valid(frame_size == ZSTD_CONTENTSIZE_UNKNOWN || frame_size == uncompressed_size,
                  "compressed frame size disagrees with header");
    const auto frame_bytes = ZSTD_findFrameCompressedSize(compressed.data(), compressed.size());
    require_valid(!ZSTD_isError(frame_bytes) && frame_bytes == compressed.size(),
                  "truncated or trailing compressed data");

    payload.resize(static_cast<std::size_t>(uncompressed_size));
    const std::size_t actual_size =
        ZSTD_decompress(payload.data(), payload.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(actual_size))
      throw std::runtime_error("abstract_tree_codec: zstd decompress failed for " + path.string() +
                               ": " + std::string(ZSTD_getErrorName(actual_size)));
    require_valid(actual_size == uncompressed_size, "decompressed size disagrees with header");
  } else {
    throw std::runtime_error("abstract_tree_codec: bad magic in " + path.string());
  }

  // Parse the payload with a fast cursor (memcpy reads).
  const std::string ctx = path.string();
  Cursor cursor(payload.data(), payload.size(), ctx);

  // Header
  char payload_magic[4];
  cursor.read_bytes(payload_magic, 4);
  if (std::memcmp(payload_magic, kMagic, 4) != 0)
    throw std::runtime_error("abstract_tree_codec: bad payload magic in " + path.string());

  const std::uint32_t payload_version = cursor.read<std::uint32_t>();
  if (payload_version != kVersion)
    throw std::runtime_error("abstract_tree_codec: unsupported version " +
                             std::to_string(payload_version) + " in " + path.string());

  const std::uint32_t node_count = cursor.read<std::uint32_t>();
  const std::uint32_t terminal_count = cursor.read<std::uint32_t>();
  const std::uint32_t depth = cursor.read<std::uint32_t>();
  const std::uint64_t accounted_bytes = cursor.read<std::uint64_t>();
  require_valid(cursor.read<std::uint32_t>() == 0, "invalid reserved header field");

  if (node_count == 0)
    throw std::runtime_error("abstract_tree_codec: zero node count in " + path.string());
  require_valid(terminal_count <= node_count && depth > 0 && depth <= node_count,
                "inconsistent tree header counts");
  if (node_count > limits.max_nodes || depth > limits.max_depth)
    throw tree_resource_exhausted("abstract tree load exceeded the node or depth cap");
  if (accounted_bytes > limits.max_bytes)
    throw tree_resource_exhausted("abstract tree declared accounting exceeded the byte cap");
  require_valid(node_count <= cursor.remaining() / 24, "truncated node headers");
  require_valid(terminal_count <= (cursor.remaining() - std::size_t{node_count} * 24) / 28,
                "truncated terminal headers");
  const GameState root(def);
  LoadBudget budget(limits.max_bytes);
  budget.charge(node_count, sizeof(TreeNode));
  budget.charge(terminal_count, sizeof(TerminalPayload));

  // Construct tree without building
  AbstractTree tree(std::move(def), std::move(action), limits, no_build_tag{});
  tree.nodes_.resize(node_count);
  tree.terminals_.resize(terminal_count);
  tree.depth_ = depth;
  std::uint32_t next_terminal = 0;

  // Read nodes — all fields via cursor (memcpy, no istringstream).
  for (std::uint32_t i = 0; i < node_count; ++i) {
    TreeNode& node = tree.nodes_[i];
    node.index = i;

    const std::uint8_t kind = cursor.read<std::uint8_t>();
    const std::uint8_t street = cursor.read<std::uint8_t>();
    const std::uint8_t action_count = cursor.read<std::uint8_t>();
    const std::uint8_t in_action_type = cursor.read<std::uint8_t>();
    const std::uint32_t actor = cursor.read<std::uint32_t>();
    const std::int32_t incoming_card = cursor.read<std::int32_t>();
    const std::uint32_t terminal = cursor.read<std::uint32_t>();
    const std::uint32_t child_count = cursor.read<std::uint32_t>();
    const std::uint32_t in_action_target = cursor.read<std::uint32_t>();

    node.kind = byte_to_node_kind(kind);
    require_valid(street <= static_cast<std::uint8_t>(Street::Preflop), "invalid node street");
    node.street = static_cast<Street>(street);
    node.actor = (actor == kNoNodeU32) ? kNoNode : static_cast<std::size_t>(actor);
    node.incoming_action =
        Action{byte_to_action_type(in_action_type), static_cast<poker::Chips>(in_action_target)};
    node.incoming_card = incoming_card;
    node.terminal = (terminal == kNoNodeU32) ? kNoNode : static_cast<std::size_t>(terminal);
    require_valid(incoming_card >= kNoCard && incoming_card < 52, "invalid incoming card");
    require_valid(aggressive(node.incoming_action) ? in_action_target > 0 : in_action_target == 0,
                  "invalid incoming action target");
    if (node.is_action()) {
      require_valid(node.actor < tree.def_.player_count && action_count > 0 && action_count <= 32 &&
                        child_count == action_count && terminal == kNoNodeU32,
                    "invalid action node shape");
    } else {
      require_valid(actor == kNoNodeU32 && action_count == 0, "invalid non-action node shape");
      if (node.is_chance()) {
        require_valid(child_count >= 48 && child_count <= 52 && terminal == kNoNodeU32,
                      "invalid chance node shape");
      } else {
        require_valid(child_count == 0, "leaf has children");
        if (node.is_terminal()) {
          require_valid(terminal == next_terminal && terminal < terminal_count,
                        "invalid terminal index");
          ++next_terminal;
        } else {
          require_valid(terminal == kNoNodeU32, "flop-deal leaf has terminal payload");
        }
      }
    }
    require_valid(child_count <= cursor.remaining() / sizeof(std::uint32_t), "truncated children");
    require_valid(
        action_count <= (cursor.remaining() - std::size_t{child_count} * sizeof(std::uint32_t)) / 5,
        "truncated actions");

    budget.charge(child_count, sizeof(std::size_t));
    node.children.resize(child_count);
    for (std::uint32_t c = 0; c < child_count; ++c)
      node.children[c] = cursor.read<std::uint32_t>();

    budget.charge(action_count, sizeof(Action));
    node.actions.resize(action_count);
    for (std::uint8_t a = 0; a < action_count; ++a) {
      const std::uint8_t type = cursor.read<std::uint8_t>();
      const std::uint32_t target = cursor.read<std::uint32_t>();
      node.actions[a] = Action{byte_to_action_type(type), static_cast<poker::Chips>(target)};
      require_valid(aggressive(node.actions[a]) ? target > 0 : target == 0,
                    "invalid action target");
    }
  }

  // Set parents from children
  for (std::uint32_t i = 0; i < node_count; ++i) {
    for (std::size_t child : tree.nodes_[i].children) {
      if (child <= i || child >= node_count || tree.nodes_[child].parent != kNoNode)
        throw std::runtime_error("abstract_tree_codec: child index " + std::to_string(child) +
                                 " is not a unique forward edge in " + path.string());
      tree.nodes_[child].parent = i;
    }
  }
  require_valid(next_terminal == terminal_count, "unreferenced terminal payloads");
  for (std::uint32_t i = 1; i < node_count; ++i)
    require_valid(tree.nodes_[i].parent != kNoNode, "unreachable node");

  // Terminal payloads
  for (std::uint32_t t = 0; t < terminal_count; ++t)
    tree.terminals_[t] = deserialize_terminal(cursor, root, budget);
  require_valid(cursor.remaining() == 0, "trailing payload bytes");

  // Replay the native transition machine on a bounded explicit stack. Local
  // ledger conservation alone cannot prove a terminal belongs to this path.
  struct Frame {
    std::size_t index;
    std::size_t next_child;
    GameState state;
  };
  if (depth > limits.max_bytes / sizeof(Frame))
    throw tree_resource_exhausted("abstract tree validation stack exceeded the byte cap");
  std::vector<Frame> stack;
  stack.reserve(depth);
  stack.push_back({0, 0, root});
  require_valid(
      tree.nodes_[0].incoming_card == kNoCard && tree.nodes_[0].incoming_action == Action{},
      "root has incoming edge label");
  std::size_t actual_depth = 1;
  while (!stack.empty()) {
    Frame& frame = stack.back();
    const TreeNode& node = tree.nodes_[frame.index];
    if (frame.next_child == 0) {
      NodeKind expected_kind = NodeKind::Action;
      switch (frame.state.phase()) {
        case poker::Phase::Action:
          expected_kind = NodeKind::Action;
          break;
        case poker::Phase::Deal:
          expected_kind = tree.def_.terminal == poker::TerminalDepth::Flop ? NodeKind::FlopDeal
                                                                           : NodeKind::Chance;
          break;
        case poker::Phase::Folded:
          expected_kind = NodeKind::TerminalFold;
          break;
        case poker::Phase::Showdown:
          expected_kind = NodeKind::TerminalShowdown;
          break;
        case poker::Phase::Frontier:
          expected_kind = NodeKind::TerminalFrontier;
          break;
      }
      require_valid(node.kind == expected_kind && node.street == frame.state.street(),
                    "node kind or street disagrees with replayed state");
      if (node.is_action()) {
        require_valid(node.actor == frame.state.actor(), "actor disagrees with replayed state");
        const auto expected_menu =
            abstract_node_menu(frame.state, tree.action_, frame.state.legal());
        require_valid(node.actions == expected_menu, "action menu disagrees with replayed state");
      }
      if (node.is_chance()) {
        const auto cards = public_runout_cards(frame.state);
        require_valid(node.children.size() == cards.size(), "chance degree disagrees with board");
        for (std::size_t ordinal = 0; ordinal < cards.size(); ++ordinal)
          require_valid(tree.nodes_[node.children[ordinal]].incoming_card == cards[ordinal],
                        "invalid chance card order");
      } else if (node.is_terminal()) {
        const auto& term = tree.terminals_[node.terminal];
        const auto expected =
            make_terminal_payload(frame.state, node.kind == NodeKind::TerminalFold);
        require_valid(
            term.player_count == expected.player_count && term.folded == expected.folded &&
                term.board_size == expected.board_size &&
                std::equal(term.board.begin(), term.board.begin() + term.board_size,
                           expected.board.begin()) &&
                term.live_count == expected.live_count &&
                std::equal(term.live_order.begin(), term.live_order.begin() + term.live_count,
                           expected.live_order.begin()) &&
                term.chip_utility == expected.chip_utility,
            "terminal payload disagrees with replayed state");
        for (std::size_t seat = 0; seat < term.player_count; ++seat)
          require_valid(term.seats[seat].contributed == expected.seats[seat].contributed &&
                            term.seats[seat].refunded == expected.seats[seat].refunded &&
                            term.seats[seat].stack == expected.seats[seat].stack &&
                            term.seats[seat].folded == expected.seats[seat].folded,
                        "terminal seat ledger disagrees with replayed state");
      }
    }
    if (frame.next_child == node.children.size()) {
      stack.pop_back();
      continue;
    }
    const auto ordinal = frame.next_child++;
    const auto child_index = node.children[ordinal];
    const auto& child = tree.nodes_[child_index];
    Frame next{child_index, 0, frame.state};
    if (node.is_action()) {
      require_valid(child.incoming_card == kNoCard &&
                        child.incoming_action.type == node.actions[ordinal].type &&
                        child.incoming_action.target_total == node.actions[ordinal].target_total,
                    "action edge disagrees with menu");
      next.state = frame.state.after_action(node.actor, node.actions[ordinal]);
    } else {
      require_valid(child.incoming_action == Action{}, "chance edge has action label");
      next.state = frame.state.after_card(child.incoming_card);
    }
    if (stack.size() >= limits.max_depth)
      throw tree_resource_exhausted("abstract tree actual depth exceeded the depth cap");
    require_valid(stack.size() < depth, "actual tree depth exceeds header");
    stack.push_back(next);
    actual_depth = std::max(actual_depth, stack.size());
  }
  require_valid(actual_depth == depth, "actual tree depth disagrees with header");
  tree.accounted_bytes_ = budget.bytes();

  return tree;
}

std::string tree_file_name(std::span<const int> flop_class, std::uint8_t spr_bucket) {
  return "tree_" + std::to_string(flop_class[0]) + "_" + std::to_string(flop_class[1]) + "_" +
         std::to_string(flop_class[2]) + "_b" + std::to_string(spr_bucket) + ".atf";
}

}  // namespace bs::tree
