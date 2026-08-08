#ifndef REBOOT2_COMMAND_LOSS_TIMER_H_
#define REBOOT2_COMMAND_LOSS_TIMER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "auth_token.h"
#include "wall_clock.h"

// Command-loss timer (CLT): the layer that assumes all software above it is
// wrong (fixer ADR 0055 §4). Counts elapsed time since the last
// AUTHENTICATED home-contact token — never link-layer traffic, never an
// unauthenticated request — and escalates through fixed rungs. Every rung
// action is chosen to be safe to fire on a false positive (Voyager rule):
// forcing WAN relays on, power-cycling, or holding a known daily posture are
// all reversible and never strand the vessel worse than "no home contact"
// already implies.
//
// This module is pure C++ (no Arduino/SensESP deps) so it can be exercised
// by a plain g++ host test with an accelerated fake clock — see
// test/test_command_loss_timer.cpp. The caller owns all I/O: applying
// Actions to real relays, feeding NTP syncs, and deciding which incoming
// requests are even eligible to be checked as tokens (link-layer traffic
// must never reach process_token() at all).
namespace reboot2::clt {

enum class Rung { kNormal, kRung12h, kRung24h, kTerminal };

enum class Action {
  kNone,
  kForceWanRelaysOn,          // T+12h entry: relays 1-4 -> powered ON
  kPowerCycleStarlinkAndPep,  // T+24h entry: NC-safe 60s cycle, fires once
  kEnterTerminalPosture,      // T+48h entry
  kExitToNormal,              // valid token seen from any rung, incl. terminal
  kTerminalWindowOpen,        // terminal posture: entering the daily UTC window
  kTerminalWindowClose,       // terminal posture: leaving the daily UTC window
};

struct Thresholds {
  uint64_t rung_12h_secs = 12 * 3600;
  uint64_t rung_24h_secs = 24 * 3600;
  uint64_t rung_48h_secs = 48 * 3600;
  uint64_t window_open_utc_secs = 16 * 3600;   // 16:00 UTC
  uint64_t window_close_utc_secs = 18 * 3600;  // 18:00 UTC
  uint64_t auth_window_secs = 600;             // +-10 min replay window
};

inline const std::string& contact_token_context() {
  static const std::string kContext = "reboot2.clt.contact";
  return kContext;
}

class CommandLossTimer {
 public:
  explicit CommandLossTimer(std::string secret, Thresholds th = Thresholds())
      : secret_(std::move(secret)), th_(th) {}

  // Call once at startup with the boot-time monotonic reading; the CLT
  // treats boot as an implicit "last contact" so a freshly-booted device
  // gets the full 12h grace period rather than starting mid-rung.
  void begin(uint64_t now_mono) { last_contact_mono_ = now_mono; }

  // Feed an NTP sync as it happens (or a manual one on the bench).
  void sync_wall_clock(uint64_t unix_seconds, uint64_t now_mono) {
    wall_clock_.sync(unix_seconds, now_mono);
  }

  // Verifies `token` as an authenticated home-contact token. On success,
  // resets elapsed time to zero (the next tick() will observe kExitToNormal
  // if a rung had fired) and returns true. `now_wall` is the caller's best
  // wall-clock estimate (NTP if available; the token's own timestamp check
  // still enforces freshness even without a synced wall clock, since the
  // token carries its own timestamp).
  bool process_token(const auth::Token& token, uint64_t now_wall, uint64_t now_mono) {
    if (!auth::verify_and_accept(token, contact_token_context(), secret_, now_wall,
                                  th_.auth_window_secs, last_accepted_token_ts_)) {
      return false;
    }
    last_contact_mono_ = now_mono;
    return true;
  }

  Rung rung() const { return rung_; }
  bool terminal_window_open() const { return terminal_window_open_; }

  // Advances the state machine. now_mono drives elapsed-since-contact and
  // the terminal window's drift-fallback estimate; call on every loop tick
  // (cheap — pure arithmetic, no I/O).
  std::vector<Action> tick(uint64_t now_mono) {
    std::vector<Action> actions;
    uint64_t elapsed = now_mono - last_contact_mono_;

    Rung target = Rung::kNormal;
    if (elapsed >= th_.rung_48h_secs) {
      target = Rung::kTerminal;
    } else if (elapsed >= th_.rung_24h_secs) {
      target = Rung::kRung24h;
    } else if (elapsed >= th_.rung_12h_secs) {
      target = Rung::kRung12h;
    }

    if (target != rung_) {
      switch (target) {
        case Rung::kNormal:
          actions.push_back(Action::kExitToNormal);
          if (terminal_window_open_) {
            terminal_window_open_ = false;
            actions.push_back(Action::kTerminalWindowClose);
          }
          break;
        case Rung::kRung12h:
          actions.push_back(Action::kForceWanRelaysOn);
          break;
        case Rung::kRung24h:
          actions.push_back(Action::kPowerCycleStarlinkAndPep);
          break;
        case Rung::kTerminal:
          actions.push_back(Action::kEnterTerminalPosture);
          break;
      }
      rung_ = target;
    }

    if (rung_ == Rung::kTerminal && wall_clock_.has_estimate()) {
      uint64_t now_unix = wall_clock_.estimate(now_mono);
      uint64_t sec_of_day = now_unix % 86400;
      bool in_window =
          sec_of_day >= th_.window_open_utc_secs && sec_of_day < th_.window_close_utc_secs;
      if (in_window && !terminal_window_open_) {
        terminal_window_open_ = true;
        actions.push_back(Action::kTerminalWindowOpen);
      } else if (!in_window && terminal_window_open_) {
        terminal_window_open_ = false;
        actions.push_back(Action::kTerminalWindowClose);
      }
    }
    // If terminal posture is entered before any NTP sync has ever occurred,
    // there is no wall-clock estimate to evaluate the window against; hold
    // last known window state rather than guess. Once a sync lands, the
    // next tick() picks the window logic up normally.

    return actions;
  }

 private:
  std::string secret_;
  Thresholds th_;
  time::WallClock wall_clock_;

  uint64_t last_contact_mono_ = 0;
  uint64_t last_accepted_token_ts_ = 0;
  Rung rung_ = Rung::kNormal;
  bool terminal_window_open_ = false;
};

}  // namespace reboot2::clt

#endif  // REBOOT2_COMMAND_LOSS_TIMER_H_
