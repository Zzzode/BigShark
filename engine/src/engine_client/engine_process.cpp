#include "engine_process.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "frame_stream.hpp"

namespace bs::engine_client {

namespace {

// Sends SIGPIPE to /dev/null for the whole process. Called once in start();
// the leaf CLI is the only consumer, so a global disposition is fine. Without
// this a write to a dead engine pipe would kill the practice simulator.
void ignore_sigpipe() {
  static bool installed = false;
  if (!installed) {
    std::signal(SIGPIPE, SIG_IGN);
    installed = true;
  }
}

}  // namespace

struct EngineProcess::Impl {
  explicit Impl(EngineClientConfig cfg) : config(std::move(cfg)) {}

  EngineClientConfig config;
  pid_t pid = -1;
  int stdin_fd = -1;   // parent writes to child's stdin
  int stdout_fd = -1;  // parent reads from child's stdout
  std::uint32_t minor = 0;
  pv::GetCapabilitiesResponse caps;
  std::uint64_t request_counter = 0;
  TransactError last_error = TransactError::None;

  ~Impl() { stop(); }

  bool spawn() {
    int in_pipe[2] = {-1, -1};   // parent -> child stdin
    int out_pipe[2] = {-1, -1};  // child stdout -> parent
    if (::pipe(in_pipe) != 0)
      return false;
    if (::pipe(out_pipe) != 0) {
      ::close(in_pipe[0]);
      ::close(in_pipe[1]);
      return false;
    }

    const pid_t child = ::fork();
    if (child < 0) {
      ::close(in_pipe[0]);
      ::close(in_pipe[1]);
      ::close(out_pipe[0]);
      ::close(out_pipe[1]);
      return false;
    }

    if (child == 0) {
      // Child: wire pipes to stdin/stdout, close unused ends, exec.
      // Safe from fd-collision with 0/1/2: the leaf CLI keeps stdin/stdout/
      // stderr open for its whole lifetime, so pipe() always returns fds >= 3.
      ::dup2(in_pipe[0], STDIN_FILENO);
      ::dup2(out_pipe[1], STDOUT_FILENO);
      ::close(in_pipe[0]);
      ::close(in_pipe[1]);
      ::close(out_pipe[0]);
      ::close(out_pipe[1]);

      std::vector<std::string> args;
      args.push_back(config.engine_path);
      args.push_back("--serve-proto");
      for (const auto& root : config.resident_roots) {
        args.push_back("--resident-root");
        args.push_back(root);
      }
      std::vector<char*> argv;
      argv.reserve(args.size() + 1);
      for (auto& a : args)
        argv.push_back(a.data());
      argv.push_back(nullptr);
      ::execv(config.engine_path.c_str(), argv.data());
      // If exec fails, exit with a distinctive code.
      _exit(127);
    }

    // Parent: keep the write end of stdin and the read end of stdout.
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    pid = child;
    stdin_fd = in_pipe[1];
    stdout_fd = out_pipe[0];
    return true;
  }

  void kill() {
    if (pid > 0) {
      ::kill(pid, SIGKILL);
      int status = 0;
      ::waitpid(pid, &status, 0);
      pid = -1;
    }
    if (stdin_fd >= 0) {
      ::close(stdin_fd);
      stdin_fd = -1;
    }
    if (stdout_fd >= 0) {
      ::close(stdout_fd);
      stdout_fd = -1;
    }
  }

  void stop() {
    if (pid <= 0) {
      if (stdin_fd >= 0)
        ::close(stdin_fd);
      if (stdout_fd >= 0)
        ::close(stdout_fd);
      stdin_fd = -1;
      stdout_fd = -1;
      return;
    }
    ::kill(pid, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int status = 0;
    bool reaped = false;
    while (std::chrono::steady_clock::now() < deadline) {
      const pid_t rc = ::waitpid(pid, &status, WNOHANG);
      if (rc == pid) {
        reaped = true;
        break;
      }
      if (rc < 0 && errno == ECHILD) {
        // The child was already reaped (e.g. by a signal handler); the PID
        // may have been recycled. Treat as done so the SIGKILL fallback
        // below does not signal an unrelated process.
        reaped = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!reaped && pid > 0) {
      ::kill(pid, SIGKILL);
      ::waitpid(pid, &status, 0);
    }
    pid = -1;
    if (stdin_fd >= 0) {
      ::close(stdin_fd);
      stdin_fd = -1;
    }
    if (stdout_fd >= 0) {
      ::close(stdout_fd);
      stdout_fd = -1;
    }
  }

  // Sends `request` and reads one response frame. Returns nullptr on any
  // failure. A timeout kills the process.
  std::unique_ptr<pv::Envelope> transact(const pv::Envelope& request) {
    last_error = TransactError::None;
    if (stdin_fd < 0 || stdout_fd < 0) {
      last_error = TransactError::NotRunning;
      return nullptr;
    }

    std::string payload;
    if (!request.SerializeToString(&payload)) {
      last_error = TransactError::MalformedFrame;
      return nullptr;
    }
    std::string frame;
    if (!encode_frame(frame, payload)) {
      last_error = TransactError::MalformedFrame;
      return nullptr;
    }
    if (!write_frame(stdin_fd, frame)) {
      last_error = TransactError::WriteFailed;
      kill();
      return nullptr;
    }

    std::string response_frame;
    const FrameStatus status =
        read_frame(stdout_fd, response_frame, static_cast<int>(config.timeout_ms));
    if (status != FrameStatus::Complete) {
      switch (status) {
        case FrameStatus::Timeout:
          last_error = TransactError::Timeout;
          kill();  // may be wedged in a solve
          break;
        case FrameStatus::EndOfStream:
          last_error = TransactError::EndOfStream;
          kill();  // process closed stdout; it is dead
          break;
        default:
          last_error = TransactError::MalformedFrame;
          // A bad varint or oversize payload desyncs the byte stream (the
          // offending bytes are still in the pipe); kill so the next
          // decision respawns a clean process.
          kill();
          break;
      }
      return nullptr;
    }

    auto response = std::make_unique<pv::Envelope>();
    if (!response->ParseFromString(response_frame)) {
      last_error = TransactError::MalformedFrame;
      // A frame that reads cleanly but is not a valid Envelope indicates a
      // protocol desync; kill so the next decision respawns a clean process.
      kill();
      return nullptr;
    }
    return response;
  }

  // Performs the capabilities handshake at the given minor. Returns true on
  // success and populates caps + minor. A rejection (UNSUPPORTED_PROTOCOL for
  // an older engine, or any other error) returns false.
  bool handshake_at(std::uint32_t try_minor) {
    pv::Envelope request;
    request.set_protocol_minor(try_minor);
    request.set_request_id("cap-" + std::to_string(++request_counter));
    request.mutable_get_capabilities_request();

    auto response = transact(request);
    if (!response)
      return false;

    if (response->has_get_capabilities_response()) {
      caps = response->get_capabilities_response();
      minor = try_minor;
      return true;
    }
    // The host wraps errors in a DecisionResponse (there is no error field on
    // Envelope). UNSUPPORTED_PROTOCOL means the engine predates this minor.
    return false;
  }
};

EngineProcess::EngineProcess(EngineClientConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

EngineProcess::~EngineProcess() = default;

bool EngineProcess::start() {
  ignore_sigpipe();
  impl_->stop();  // clean up any previous process
  if (!impl_->spawn())
    return false;

  // Try minor 2 first (resident blueprint + terminal-only resolver + labeled
  // operational fallback). On any failure, retry at minor 0 (blueprint +
  // heuristic fallthrough).
  if (impl_->handshake_at(2))
    return true;
  impl_->kill();
  if (!impl_->spawn())
    return false;
  if (impl_->handshake_at(0))
    return true;
  impl_->kill();
  return false;
}

void EngineProcess::stop() {
  impl_->stop();
}

bool EngineProcess::alive() const noexcept {
  return impl_->pid > 0;
}

std::unique_ptr<pv::Envelope> EngineProcess::round_trip(const pv::Envelope& request) {
  return impl_->transact(request);
}

TransactError EngineProcess::last_error() const noexcept {
  return impl_->last_error;
}

std::uint32_t EngineProcess::negotiated_minor() const noexcept {
  return impl_->minor;
}

const pv::GetCapabilitiesResponse& EngineProcess::capabilities() const noexcept {
  return impl_->caps;
}

}  // namespace bs::engine_client
