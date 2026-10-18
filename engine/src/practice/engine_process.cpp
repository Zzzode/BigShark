// practice/engine_process.cpp — C++ v2 engine process client implementation.
#include "bs/practice/engine_process.hpp"

#include <bigshark/engine/v2/engine.pb.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <istream>
#include <ostream>
#include <string>
#include <thread>

namespace bs::practice {
namespace {

// Write the entire buffer to the fd, retrying on EINTR.
bool write_all(int fd, const std::string& data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t n = ::write(fd, data.data() + offset, data.size() - offset);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    offset += static_cast<std::size_t>(n);
  }
  return true;
}

// Read exactly `size` bytes from the fd, retrying on EINTR.
bool read_all(int fd, char* buf, std::size_t size) {
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t n = ::read(fd, buf + offset, size - offset);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (n == 0)
      return false;  // unexpected EOF
    offset += static_cast<std::size_t>(n);
  }
  return true;
}

// Read one ULEB128-framed payload from the fd. Returns the payload string,
// or empty on error/EOF.
std::string read_frame(int fd) {
  // Decode the ULEB128 length prefix (max 5 bytes).
  std::uint64_t length = 0;
  int prefix_bytes = 0;
  while (true) {
    unsigned char byte;
    if (!read_all(fd, reinterpret_cast<char*>(&byte), 1))
      return {};
    if (prefix_bytes >= 5)
      return {};  // overlong
    length |= static_cast<std::uint64_t>(byte & 0x7f) << (7 * prefix_bytes);
    ++prefix_bytes;
    if (length > bs::kMaxFrameBytes)
      return {};  // oversize
    if ((byte & 0x80) == 0)
      break;
  }
  if (length == 0)
    return {};
  std::string payload(static_cast<std::size_t>(length), '\0');
  if (!read_all(fd, payload.data(), payload.size()))
    return {};
  return payload;
}

}  // namespace

EngineProcess::EngineProcess(EngineProcessConfig config) : config_(std::move(config)) {}

EngineProcess::~EngineProcess() {
  stop();
}

bool EngineProcess::start() {
  if (running())
    return true;

  // Create stdin/stdout pipes.
  int in_pipe[2], out_pipe[2];
  if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0) {
    last_error_ = "pipe() failed: ";
    last_error_ += std::strerror(errno);
    return false;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    last_error_ = "fork() failed: ";
    last_error_ += std::strerror(errno);
    ::close(in_pipe[0]);
    ::close(in_pipe[1]);
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    return false;
  }

  if (pid == 0) {
    // Child: redirect stdin/stdout to the pipes.
    ::dup2(in_pipe[0], STDIN_FILENO);
    ::dup2(out_pipe[1], STDOUT_FILENO);
    ::close(in_pipe[0]);
    ::close(in_pipe[1]);
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    // stderr is inherited so the host's diagnostics go to the terminal.

    // Build the argv.
    std::vector<std::string> args;
    args.push_back(config_.engine_path);
    args.push_back("--serve-proto");
    for (const auto& root : config_.resident_roots) {
      args.push_back("--resident-root");
      args.push_back(root);
    }
    if (!config_.flop_library.empty()) {
      args.push_back("--flop-library");
      args.push_back(config_.flop_library);
    }
    if (config_.lazy)
      args.push_back("--lazy");

    std::vector<char*> argv;
    for (auto& a : args)
      argv.push_back(a.data());
    argv.push_back(nullptr);
    ::execv(config_.engine_path.c_str(), argv.data());
    // If exec fails, exit with an error code.
    _exit(127);
  }

  // Parent: close the child's ends.
  ::close(in_pipe[0]);
  ::close(out_pipe[1]);
  pid_ = pid;
  stdin_fd_ = in_pipe[1];
  stdout_fd_ = out_pipe[0];

  // Capabilities handshake: send GetCapabilitiesRequest.
  pv2::Envelope request;
  request.set_protocol_version(3);
  request.set_request_id("cap-0");
  request.mutable_get_capabilities_request();

  auto response = this->request(request);
  if (!response) {
    last_error_ = "capabilities handshake failed: " + last_error_;
    stop();
    return false;
  }

  if (!response->has_get_capabilities_response()) {
    last_error_ = "capabilities handshake: unexpected response type";
    stop();
    return false;
  }

  capabilities_ =
      std::make_unique<pv2::GetCapabilitiesResponse>(response->get_capabilities_response());
  return true;
}

std::unique_ptr<pv2::Envelope> EngineProcess::request(const pv2::Envelope& req) {
  if (!running()) {
    last_error_ = "engine process not running";
    return nullptr;
  }

  // Serialize and frame the request.
  std::string payload;
  if (!req.SerializeToString(&payload)) {
    last_error_ = "failed to serialize request envelope";
    return nullptr;
  }
  std::string frame;
  if (!bs::writeFrame(frame, payload)) {
    last_error_ = "frame too large";
    return nullptr;
  }
  if (!write_all(stdin_fd_, frame)) {
    last_error_ = "write failed: ";
    last_error_ += std::strerror(errno);
    return nullptr;
  }

  // Poll for the response with a timeout.
  struct pollfd pfd;
  pfd.fd = stdout_fd_;
  pfd.events = POLLIN;
  const int timeout_ms = static_cast<int>(config_.timeout_ms);
  const int poll_result = ::poll(&pfd, 1, timeout_ms);
  if (poll_result < 0) {
    last_error_ = "poll() failed: ";
    last_error_ += std::strerror(errno);
    return nullptr;
  }
  if (poll_result == 0) {
    last_error_ = "timeout waiting for engine response";
    return nullptr;
  }
  if (!(pfd.revents & POLLIN)) {
    last_error_ = "engine stdout closed or error";
    return nullptr;
  }

  // Read the response frame.
  const std::string response_payload = read_frame(stdout_fd_);
  if (response_payload.empty()) {
    last_error_ = "failed to read response frame";
    return nullptr;
  }

  auto response = std::make_unique<pv2::Envelope>();
  if (!response->ParseFromString(response_payload)) {
    last_error_ = "failed to parse response envelope";
    return nullptr;
  }

  // Correlate by request_id.
  if (response->request_id() != req.request_id()) {
    last_error_ =
        "request_id mismatch: expected " + req.request_id() + ", got " + response->request_id();
    return nullptr;
  }

  return response;
}

void EngineProcess::stop() {
  if (pid_ > 0) {
    ::kill(pid_, SIGTERM);
    int status = 0;
    // Wait for the process to exit, with a short grace period.
    for (int i = 0; i < 10; ++i) {
      if (::waitpid(pid_, &status, WNOHANG) == pid_)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (::waitpid(pid_, &status, WNOHANG) == 0) {
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, &status, 0);
    }
    pid_ = -1;
  }
  if (stdin_fd_ >= 0) {
    ::close(stdin_fd_);
    stdin_fd_ = -1;
  }
  if (stdout_fd_ >= 0) {
    ::close(stdout_fd_);
    stdout_fd_ = -1;
  }
}

const pv2::GetCapabilitiesResponse& EngineProcess::capabilities() const noexcept {
  static const pv2::GetCapabilitiesResponse empty;
  return capabilities_ ? *capabilities_ : empty;
}

}  // namespace bs::practice
