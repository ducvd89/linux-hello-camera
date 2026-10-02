#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace lhc {

// Per-user brake on face attempts, in memory only: after `max_failures` failures within `window`
// the user is blocked for `block`. A match clears the user's failures.
class RateLimiter {
 public:
  using Clock = std::chrono::steady_clock;

  RateLimiter(int max_failures = 5, std::chrono::seconds window = std::chrono::minutes(5),
              std::chrono::seconds block = std::chrono::minutes(2))
      : max_failures_(max_failures), window_(window), block_(block) {}

  void recordFailure(const std::string& user, Clock::time_point now);
  void recordSuccess(const std::string& user);

  bool blocked(const std::string& user, Clock::time_point now);

  // True while any user has a failure that has not aged out or is blocked: the daemon stays up so
  // the counters are not lost.
  bool hasRecentFailures(Clock::time_point now);

 private:
  struct State {
    std::vector<Clock::time_point> failures;
    Clock::time_point blocked_until{};
  };

  void prune(State& state, Clock::time_point now) const;

  int max_failures_;
  std::chrono::seconds window_;
  std::chrono::seconds block_;
  std::mutex mutex_;
  std::map<std::string, State> users_;
};

}  // namespace lhc
