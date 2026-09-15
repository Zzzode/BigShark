#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <bs/strategy_artifact.hpp>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "artifact_internal.hpp"

namespace bs::artifacts::detail {

namespace {

void throw_errno(const std::string& operation, int error) {
  if (error == 0)
    error = EIO;
  throw ArtifactError(ArtifactErrorKind::Io, operation + ": " + std::strerror(error) + " (errno " +
                                                 std::to_string(error) + ")");
}

}  // namespace

std::uint64_t file_size_required(const std::filesystem::path& path, std::uint64_t max_file_bytes) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec)
    throw ArtifactError(ArtifactErrorKind::Io,
                        "cannot stat artifact " + path.string() + ": " + ec.message());
  if (static_cast<std::uint64_t>(size) > max_file_bytes)
    throw ArtifactError(ArtifactErrorKind::FileTooLarge,
                        "artifact " + path.string() + " is " + std::to_string(size) +
                            " bytes, exceeding the " + std::to_string(max_file_bytes) +
                            " byte bound");
  return static_cast<std::uint64_t>(size);
}

namespace {

std::uint16_t read_be16(const unsigned char* bytes) {
  return static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]);
}

std::uint32_t read_be32(const unsigned char* bytes) {
  return (static_cast<std::uint32_t>(bytes[0]) << 24) |
         (static_cast<std::uint32_t>(bytes[1]) << 16) |
         (static_cast<std::uint32_t>(bytes[2]) << 8) | static_cast<std::uint32_t>(bytes[3]);
}

}  // namespace

void validate_database_header(const std::filesystem::path& path, std::uint64_t file_bytes) {
  // The SQLite header is authoritative for the magic, page size, and the
  // in-header database size, which is refreshed only at commit time. A torn
  // transaction whose rollback journal was removed can leave trailing pages
  // beyond the committed page count (or a short file below it); reject both
  // before SQLite is asked to trust the file.
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr)
    throw ArtifactError(ArtifactErrorKind::Io, "cannot open header: " + path.string());
  unsigned char header[100]{};
  const std::size_t got = std::fread(header, 1, sizeof(header), file);
  std::fclose(file);
  if (got != sizeof(header))
    throw ArtifactError(ArtifactErrorKind::Corrupt,
                        "artifact is shorter than the 100-byte SQLite header: " + path.string());
  static constexpr unsigned char kMagic[] = {'S', 'Q', 'L', 'i', 't', 'e', ' ', 'f',
                                             'o', 'r', 'm', 'a', 't', ' ', '3', 0};
  if (std::memcmp(header, kMagic, sizeof(kMagic)) != 0)
    throw ArtifactError(ArtifactErrorKind::Corrupt,
                        "not a SQLite database (bad magic header): " + path.string());
  // Bytes 18 and 19 are the file format read/write versions. Legacy rollback
  // databases are 1; a value of 2 means the file header is in WAL mode and can
  // be transparently combined with an unhashed -wal sidecar. The artifact
  // reader only ever accepts legacy-format main files.
  if (header[18] != 1 || header[19] != 1)
    throw ArtifactError(ArtifactErrorKind::Corrupt,
                        "artifact is in WAL file format; immutable artifacts must be legacy "
                        "rollback format: " +
                            path.string());
  const std::uint16_t page_size_raw = read_be16(header + 16);
  const std::uint64_t page_size = page_size_raw == 1 ? 65536 : page_size_raw;
  if (page_size == 0 || (page_size & (page_size - 1)) != 0 || page_size < 512 || page_size > 65536)
    throw ArtifactError(ArtifactErrorKind::Corrupt,
                        "invalid page size in artifact header: " + path.string());
  if (file_bytes < page_size || file_bytes % page_size != 0)
    throw ArtifactError(ArtifactErrorKind::Corrupt,
                        "artifact size is not a whole number of database pages: " + path.string());
  const std::uint32_t header_pages = read_be32(header + 28);
  if (header_pages != 0) {
    const std::uint64_t actual_pages = file_bytes / page_size;
    if (static_cast<std::uint64_t>(header_pages) != actual_pages)
      throw ArtifactError(ArtifactErrorKind::Corrupt,
                          "artifact byte length disagrees with the committed in-header "
                          "database size (torn transaction or truncated file): " +
                              path.string());
  }
}

Sha256Digest sha256_file(const std::filesystem::path& path, std::uint64_t max_file_bytes) {
  const std::uint64_t size = file_size_required(path, max_file_bytes);
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw ArtifactError(ArtifactErrorKind::Io, "cannot open for hashing: " + path.string());

  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (context == nullptr)
    throw ArtifactError(ArtifactErrorKind::Io, "EVP_MD_CTX allocation failed");

  bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
  if (!ok) {
    EVP_MD_CTX_free(context);
    throw ArtifactError(ArtifactErrorKind::Io, "EVP_DigestInit_ex failed");
  }

  constexpr std::size_t kChunk = 64 * 1024;
  std::vector<char> buffer(kChunk);
  std::uint64_t seen = 0;
  while (seen < size) {
    const auto want = static_cast<std::streamsize>(std::min<std::uint64_t>(kChunk, size - seen));
    in.read(buffer.data(), want);
    const auto got = in.gcount();
    if (got <= 0) {
      EVP_MD_CTX_free(context);
      throw ArtifactError(ArtifactErrorKind::Io, "short read while hashing: " + path.string());
    }
    if (EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(got)) != 1) {
      EVP_MD_CTX_free(context);
      throw ArtifactError(ArtifactErrorKind::Io, "EVP_DigestUpdate failed");
    }
    seen += static_cast<std::uint64_t>(got);
  }

  Sha256Digest digest{};
  unsigned int length = 0;
  ok = EVP_DigestFinal_ex(context, digest.data(), &length) == 1 && length == digest.size();
  EVP_MD_CTX_free(context);
  if (!ok)
    throw ArtifactError(ArtifactErrorKind::Io, "EVP_DigestFinal_ex failed");
  return digest;
}

void reject_sqlite_sidecars(const std::filesystem::path& path) {
  static constexpr const char* kSuffixes[] = {"-wal", "-shm", "-journal"};
  std::error_code ec;
  for (const char* suffix : kSuffixes) {
    const std::filesystem::path sidecar = std::filesystem::path(path.string() + suffix);
    const bool exists = std::filesystem::exists(sidecar, ec);
    if (ec)
      throw ArtifactError(ArtifactErrorKind::Io,
                          "cannot stat artifact sidecar " + sidecar.string() + ": " + ec.message());
    if (exists)
      throw ArtifactError(
          ArtifactErrorKind::Corrupt,
          "artifact has an unhashed SQLite sidecar " + sidecar.filename().string() +
              "; refusing to read content outside the verified digest: " + path.string());
  }
}

void ensure_reader_heap_bound() {
  // sqlite3_hard_heap_limit64 is process-global in the 3.50 amalgamation and
  // bigshark_artifacts is the only consumer of this privately linked SQLite
  // copy, so setting it once for the process lifetime is the intended
  // defense-in-depth against schema parse amplification (P2-4). The limit is
  // deliberately never lowered afterwards.
  static std::once_flag once;
  std::call_once(once, [] {
    (void)sqlite3_hard_heap_limit64(static_cast<sqlite3_int64>(kReaderHeapBoundBytes));
  });
}

std::string to_hex(const Sha256Digest& digest) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text(digest.size() * 2, '0');
  for (std::size_t i = 0; i < digest.size(); ++i) {
    text[2 * i] = kDigits[digest[i] >> 4];
    text[2 * i + 1] = kDigits[digest[i] & 0x0F];
  }
  return text;
}

void fsync_file(const std::filesystem::path& path) {
  const std::string name = path.string();
  int fd = ::open(name.c_str(), O_RDONLY);
  if (fd < 0)
    throw_errno("open for fsync: " + name, errno);
  if (::fsync(fd) != 0) {
    const int error = errno;
    ::close(fd);
    throw_errno("fsync: " + name, error);
  }
  if (::close(fd) != 0)
    throw_errno("close after fsync: " + name, errno);
}

void fsync_directory(const std::filesystem::path& directory) {
  const std::string name = directory.string();
  int fd = ::open(name.c_str(), O_RDONLY);
  if (fd < 0)
    throw_errno("open directory for fsync: " + name, errno);
  if (::fsync(fd) != 0) {
    const int error = errno;
    ::close(fd);
    throw_errno("fsync directory: " + name, error);
  }
  if (::close(fd) != 0)
    throw_errno("close directory after fsync: " + name, errno);
}

void exclusive_hard_link(const std::filesystem::path& from, const std::filesystem::path& to) {
  if (::link(from.string().c_str(), to.string().c_str()) != 0) {
    const int error = errno;
    if (error == EEXIST)
      throw ArtifactError(ArtifactErrorKind::AlreadyExists,
                          "refusing to overwrite existing generation: " + to.string());
    throw_errno("exclusive link to " + to.string(), error);
  }
}

void make_read_only(const std::filesystem::path& path) {
  if (::chmod(path.string().c_str(), S_IRUSR | S_IRGRP | S_IROTH) != 0)
    throw_errno("chmod read-only: " + path.string(), errno);
}

std::filesystem::path make_unique_temp(const std::filesystem::path& directory) {
  using Clock = std::chrono::steady_clock;
  const std::string stamp =
      std::to_string(static_cast<unsigned long long>(Clock::now().time_since_epoch().count()));
  for (int attempt = 0; attempt < 1024; ++attempt) {
    const std::string suffix =
        stamp + "-" + std::to_string(::getpid()) + "-" + std::to_string(attempt);
    auto candidate = directory / (".bs-policy-" + suffix + ".tmp");
    int fd = ::open(candidate.string().c_str(), O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd >= 0) {
      ::close(fd);
      return candidate;
    }
    if (errno != EEXIST)
      throw_errno("create temporary file in " + directory.string(), errno);
  }
  throw ArtifactError(ArtifactErrorKind::Io,
                      "could not allocate a unique temporary name in " + directory.string());
}

}  // namespace bs::artifacts::detail

namespace bs::artifacts {

std::string sha256_file_hex(const std::filesystem::path& path, std::uint64_t max_file_bytes) {
  return detail::to_hex(detail::sha256_file(path, max_file_bytes));
}

}  // namespace bs::artifacts
