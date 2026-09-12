#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "cloudseed/config.h"

#if CLOUDSEED_STAGED_MEMORY
// A transport (see dsp/staging.h) that queues its copies and runs them at
// Wait(), as the MDMA does in the background: what the staging reads before
// the wait is what the rings held before the segment. Copies added while a
// segment is pending belong to the next segment.
class QueuedTransport {
 public:
  enum class Failure { kNone, kCopy, kCommit, kActiveCommit, kWait };
  Failure failure = Failure::kNone;
  bool hold_pending = false;
  bool bad_lifetime = false;
  unsigned int commits = 0, waits = 0;
  void Begin() {
    if (pending_) bad_lifetime = true;
    copies_.clear();
    committed_ = 0;
  }
  bool Copy(const float* source, float* destination, int floats) {
    if (failure == Failure::kCopy) return false;
    copies_.push_back({source, destination, floats});
    return true;
  }
  bool Commit() {
    ++commits;
    if (pending_) bad_lifetime = true;
    // MDMA Commit() can discover an unexpectedly enabled channel. It
    // rejects the submission but still owns memory until Wait() drains it.
    if (failure == Failure::kActiveCommit) {
      pending_ = true;
      return false;
    }
    if (failure == Failure::kCommit) return false;
    if (committed_ == copies_.size()) return true;
    committed_ = copies_.size();
    pending_ = true;
    return true;
  }
  bool Wait(uint32_t* spun) {
    ++waits;
    if (spun) *spun = 0;
    if (!pending_) return true;
    if (hold_pending && pending_) return false;
    pending_ = false;
    if (failure == Failure::kWait) {
      copies_.clear();
      committed_ = 0;
      return false;
    }
    for (size_t i = 0; i < committed_; i++) {
      const CopyCommand& c = copies_[i];
      std::memcpy(c.destination, c.source, c.floats * sizeof(float));
    }
    copies_.erase(copies_.begin(), copies_.begin() + committed_);
    committed_ = 0;
    return true;
  }
  bool Idle() const { return !pending_; }
  bool Done() const { return !hold_pending; }
  int Capacity() const { return 1 << 30; }

 private:
  struct CopyCommand {
    const float* source;
    float* destination;
    int floats;
  };
  std::vector<CopyCommand> copies_;
  size_t committed_ = 0;
  bool pending_ = false;
};
#endif
