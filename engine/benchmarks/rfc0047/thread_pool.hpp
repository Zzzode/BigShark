// Persistent fork-join worker pool shared by the RFC 0047 benchmark solvers.
//
// Workers persist across parallel_for calls so a CFR kernel pays the thread
// creation cost once. Every parallel region is a deterministic fork-join with
// an epoch barrier: the calling thread does not return until every worker has
// finished the region and parked at the barrier, so no worker can touch a
// region's state (function pointer, range, counters) after the caller starts
// the next region. Work is dispatched in contiguous CHUNKS rather than single
// indices, which makes synchronization O(workers) instead of O(n). The caller's
// task for index i writes only its own output and runs exactly once with its
// own serial inner reduction, so results are bitwise identical across thread
// counts (no cross-task reduction reorder). The calling thread helps drain the
// chunks. Benchmark-only under the rfc0047 link guard.
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace bs::rfc0047 {

class Pool {
 public:
  explicit Pool(std::size_t n) {
    if (n == 0)
      n = std::thread::hardware_concurrency();
    if (n == 0)
      n = 1;
    worker_count_ = n;
    for (std::size_t t = 0; t < n; ++t)
      workers_.emplace_back([this] { worker_loop(); });
  }

  ~Pool() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    work_cv_.notify_all();
    for (auto& w : workers_)
      w.join();
  }
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;

  std::size_t size() const { return worker_count_; }

  void parallel_for(std::size_t n, const std::function<void(std::size_t)>& fn) {
    if (n <= 1 || worker_count_ <= 1) {
      for (std::size_t i = 0; i < n; ++i)
        fn(i);
      return;
    }
    // Split [0,n) into a modest multiple of the worker count of contiguous
    // chunks. Each chunk is claimed with one atomic index add and completed
    // with one atomic countdown; every index still runs exactly once, so the
    // per-index outputs and the caller's serial inner reduction are unchanged
    // (bitwise identical regardless of thread count).
    const std::size_t chunks = std::min(n, worker_count_ * kChunksPerWorker);
    {
      std::lock_guard<std::mutex> lk(mu_);
      fn_ = &fn;
      n_ = n;
      chunks_ = chunks;
      next_chunk_ = 0;
      remaining_ = chunks;
      arrived_ = 0;
      ++epoch_;
    }
    work_cv_.notify_all();
    drain();  // calling thread helps; it does not count in arrived_
    std::unique_lock<std::mutex> lk(done_mu_);
    done_cv_.wait(lk, [&] { return remaining_.load() == 0 && arrived_.load() == worker_count_; });
  }

 private:
  static constexpr std::size_t kChunksPerWorker = 4;

  // Claim and run chunks of the current epoch until none remain. All member
  // state for the epoch is read under mu_ on entry, and parallel_for cannot
  // advance the epoch until every worker has passed this call and parked, so a
  // worker never acts on a stale function pointer or range.
  void drain() {
    const std::function<void(std::size_t)>* fn;
    std::size_t n;
    std::size_t chunks;
    {
      std::lock_guard<std::mutex> lk(mu_);
      fn = fn_;
      n = n_;
      chunks = chunks_;
    }
    std::size_t c;
    while ((c = next_chunk_.fetch_add(1)) < chunks) {
      const std::size_t lo = c * n / chunks;
      const std::size_t hi = (c + 1) * n / chunks;
      for (std::size_t i = lo; i < hi; ++i)
        (*fn)(i);
      if (remaining_.fetch_sub(1) == 1) {
        std::lock_guard<std::mutex> dlk(done_mu_);
        done_cv_.notify_all();
      }
    }
  }

  void worker_loop() {
    std::size_t seen = 0;
    for (;;) {
      {
        std::unique_lock<std::mutex> lk(mu_);
        work_cv_.wait(lk, [&] { return stop_ || epoch_ != seen; });
        if (stop_)
          return;
        seen = epoch_;
      }
      drain();
      // Epoch barrier arrival: park only after all of this worker's chunks are
      // done. The caller waits for arrived_ == worker_count_ before starting
      // the next epoch, guaranteeing no worker still references epoch state.
      if (arrived_.fetch_add(1) + 1 == worker_count_) {
        std::lock_guard<std::mutex> dlk(done_mu_);
        done_cv_.notify_all();
      }
    }
  }

  std::vector<std::thread> workers_;
  std::size_t worker_count_ = 0;
  std::mutex mu_;
  std::condition_variable work_cv_;
  const std::function<void(std::size_t)>* fn_ = nullptr;
  std::size_t n_ = 0;
  std::size_t chunks_ = 0;
  std::atomic<std::size_t> next_chunk_{0};
  std::atomic<std::size_t> remaining_{0};
  std::atomic<std::size_t> arrived_{0};
  std::size_t epoch_ = 0;
  bool stop_ = false;
  std::mutex done_mu_;
  std::condition_variable done_cv_;
};

}  // namespace bs::rfc0047
