#ifndef REBOOT2_COMMIT_CONFIRM_H_
#define REBOOT2_COMMIT_CONFIRM_H_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "auth_token.h"

// Commit-confirm guard (fixer ADR 0055 §4): any command that would cut power
// to a WAN-chain relay (1-4) applies provisionally and is automatically
// reverted after `confirm_window_secs` (default 15 min) unless an
// authenticated confirm token arrives for that relay. Reboot commands are
// exempt — they are already self-reverting (cut power, wait, restore). This
// is the layer that would have self-healed the 2026-08-07 incident override
// (see test/test_commit_confirm.cpp for that scenario encoded as a test).
//
// Pure C++ (no Arduino/SensESP deps), so it compiles under both the firmware
// env and a plain g++ host test with an accelerated fake clock.
namespace reboot2::confirm {

enum class Disposition {
  kExempt,              // reboot, a power-on, or a non-WAN relay: apply directly, nothing tracked
  kAppliedProvisional,  // WAN relay power-cut: apply now, but revert unless confirmed in time
};

struct Pending {
  bool prior_powered_state;
  uint64_t deadline_mono;
};

struct Revert {
  int relay_id;
  bool revert_to_powered_state;
};

inline std::string confirm_token_context(int relay_id) {
  return "reboot2.confirm.relay" + std::to_string(relay_id);
}

class CommitConfirmGuard {
 public:
  explicit CommitConfirmGuard(std::string secret, uint64_t confirm_window_secs = 15 * 60,
                               uint64_t auth_window_secs = 600)
      : secret_(std::move(secret)),
        window_secs_(confirm_window_secs),
        auth_window_secs_(auth_window_secs) {}

  static bool is_wan_relay(int relay_id) { return relay_id >= 1 && relay_id <= 4; }

  // Call for every command that changes a WAN-chain relay's powered state.
  // Only a power-off (cut) on relays 1-4 is tracked as provisional; turning
  // a relay on, any command on relays 5/6, or a reboot command (already
  // self-reverting) is exempt and applied directly.
  Disposition submit(int relay_id, bool new_powered_state, bool prior_powered_state,
                      bool is_reboot, uint64_t now_mono) {
    if (is_reboot || !is_wan_relay(relay_id) || new_powered_state) {
      pending_.erase(relay_id);
      return Disposition::kExempt;
    }
    pending_[relay_id] = Pending{prior_powered_state, now_mono + window_secs_};
    return Disposition::kAppliedProvisional;
  }

  // Authenticates `token` (scoped to this relay via confirm_token_context())
  // and, on success, confirms the pending command so it will not be
  // auto-reverted. Returns false if the token is invalid/stale/replayed, or
  // if there was nothing pending for that relay.
  bool confirm(int relay_id, const auth::Token& token, uint64_t now_wall) {
    if (!auth::verify_and_accept(token, confirm_token_context(relay_id), secret_, now_wall,
                                  auth_window_secs_, last_accepted_token_ts_[relay_id])) {
      return false;
    }
    auto it = pending_.find(relay_id);
    if (it == pending_.end()) return false;
    pending_.erase(it);
    return true;
  }

  // Call periodically. Returns the reverts to apply for any pending command
  // whose deadline passed without a confirm.
  std::vector<Revert> tick(uint64_t now_mono) {
    std::vector<Revert> reverts;
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (now_mono >= it->second.deadline_mono) {
        reverts.push_back(Revert{it->first, it->second.prior_powered_state});
        it = pending_.erase(it);
      } else {
        ++it;
      }
    }
    return reverts;
  }

  bool has_pending(int relay_id) const { return pending_.count(relay_id) > 0; }

 private:
  std::string secret_;
  uint64_t window_secs_;
  uint64_t auth_window_secs_;
  std::unordered_map<int, Pending> pending_;
  std::unordered_map<int, uint64_t> last_accepted_token_ts_;
};

}  // namespace reboot2::confirm

#endif  // REBOOT2_COMMIT_CONFIRM_H_
