#ifndef REBOOT2_INTERNET_PROBE_H_
#define REBOOT2_INTERNET_PROBE_H_

#include <cstdint>
#include <functional>

// Internet-reachability probe + action pacing (fixer ADR 0055 §4, job 3/4).
// Pure C++ (no Arduino/SensESP deps), so it compiles under both the firmware
// env and a plain g++ host test with injected fake probes/clock.
//
// The existing router watchdog (src/main.cpp) only probes NarwhalCore on the
// LAN (192.168.22.1) — it cannot see a WAN-only outage where the router
// answers fine locally but has no internet. This module adds that second
// input as a two-target consensus probe (both targets must fail before
// "internet down" is declared, so one flaky/rate-limited target can't cause
// a false read), a multi-tick confirm gate (debounces a single bad tick
// before treating an outage as real), and an exponential hold-off between
// watchdog actions that never proposes more actions than the pre-existing
// circuit breaker (`max_actions`, e.g. MAX_ROUTER_REBOOTS) allows.
namespace reboot2::netprobe {

// Result of probing two independent well-known targets (e.g. two distinct
// anycast IPs) in the same tick.
struct ConsensusResult {
  bool target_a_ok;
  bool target_b_ok;

  // Internet is declared down only when BOTH targets fail. A single
  // target's outage (regional anycast issue, rate limiting) must not by
  // itself read as "internet down".
  bool internet_down() const { return !target_a_ok && !target_b_ok; }
};

using ProbeFn = std::function<bool()>;

inline ConsensusResult check_consensus(const ProbeFn& probe_a, const ProbeFn& probe_b) {
  return ConsensusResult{probe_a(), probe_b()};
}

// Requires `required_consecutive` consecutive "down" ticks before reporting
// the condition as confirmed; any "up" tick resets the count to zero. This
// is the multi-condition confirm gate: a single bad tick (transient loss,
// one probe target hiccup) must not be enough to treat an outage as real.
class ConfirmGate {
 public:
  explicit ConfirmGate(uint8_t required_consecutive) : required_(required_consecutive) {}

  bool tick(bool down) {
    count_ = down ? static_cast<uint8_t>(count_ + 1) : 0;
    return count_ >= required_;
  }

  void reset() { count_ = 0; }

  uint8_t consecutive_count() const { return count_; }

 private:
  uint8_t required_;
  uint8_t count_ = 0;
};

// Exponential hold-off between watchdog actions: base_secs * 2^actions_taken,
// capped at max_secs. `actions_taken` is the number of watchdog actions
// already taken (0 before the first action). `max_actions` mirrors the
// pre-existing circuit breaker's action limit — once that many actions have
// been taken the breaker is open and no further action (and so no further
// hold-off) applies.
class ExponentialHoldoff {
 public:
  ExponentialHoldoff(uint32_t base_secs, uint32_t max_secs, uint8_t max_actions)
      : base_secs_(base_secs), max_secs_(max_secs), max_actions_(max_actions) {}

  bool breaker_open(uint8_t actions_taken) const { return actions_taken >= max_actions_; }

  uint32_t holdoff_secs(uint8_t actions_taken) const {
    uint64_t s = static_cast<uint64_t>(base_secs_) << actions_taken;  // base * 2^n
    if (s > max_secs_) s = max_secs_;
    return static_cast<uint32_t>(s);
  }

 private:
  uint32_t base_secs_;
  uint32_t max_secs_;
  uint8_t max_actions_;
};

}  // namespace reboot2::netprobe

#endif  // REBOOT2_INTERNET_PROBE_H_
