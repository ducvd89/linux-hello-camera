#pragma once

#include <chrono>
#include <mutex>

namespace lhc {

// Only one camera session (and one use of the loaded models) at a time. A second caller waits
// up to its timeout, then is told the daemon is busy.
class SessionGate {
 public:
  class Lock {
   public:
    Lock() = default;
    Lock(std::timed_mutex& m, std::chrono::milliseconds wait) : lock_(m, wait) {}
    explicit operator bool() const { return lock_.owns_lock(); }

   private:
    std::unique_lock<std::timed_mutex> lock_;
  };

  // Check the result: false means the wait timed out.
  Lock acquire(std::chrono::milliseconds wait) { return Lock(mutex_, wait); }

 private:
  std::timed_mutex mutex_;
};

}  // namespace lhc
