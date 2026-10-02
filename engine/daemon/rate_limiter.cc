#include "daemon/rate_limiter.h"

#include <algorithm>

namespace lhc {

void RateLimiter::prune(State& state, Clock::time_point now) const {
  state.failures.erase(std::remove_if(state.failures.begin(), state.failures.end(),
                                      [&](Clock::time_point t) { return now - t >= window_; }),
                       state.failures.end());
}

void RateLimiter::recordFailure(const std::string& user, Clock::time_point now) {
  std::lock_guard<std::mutex> lock(mutex_);
  State& state = users_[user];
  prune(state, now);
  state.failures.push_back(now);
  if (static_cast<int>(state.failures.size()) >= max_failures_) {
    state.blocked_until = now + block_;
    state.failures.clear();  // the block is the penalty; start counting afresh afterwards
  }
}

void RateLimiter::recordSuccess(const std::string& user) {
  std::lock_guard<std::mutex> lock(mutex_);
  users_.erase(user);
}

bool RateLimiter::blocked(const std::string& user, Clock::time_point now) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = users_.find(user);
  return it != users_.end() && now < it->second.blocked_until;
}

bool RateLimiter::hasRecentFailures(Clock::time_point now) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [user, state] : users_) {
    prune(state, now);
    if (!state.failures.empty() || now < state.blocked_until) {
      return true;
    }
  }
  return false;
}

}  // namespace lhc
