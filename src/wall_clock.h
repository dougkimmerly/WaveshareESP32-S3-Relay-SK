#ifndef REBOOT2_WALL_CLOCK_H_
#define REBOOT2_WALL_CLOCK_H_

#include <cstdint>

// Best-effort UTC wall clock with an NTP anchor and a monotonic-clock drift
// fallback (fixer ADR 0055 §4). Time base:
//   - while NTP is reachable, the caller feeds fresh (unix_seconds,
//     now_mono) anchors via sync(). is_ntp_synced() is true only within
//     kSyncStaleSecs of the most recent sync — that's what lets the
//     terminal-posture window logic detect "NTP lost" and switch to
//     fallback.
//   - once NTP goes stale (WAN down), estimate() extrapolates from the LAST
//     good anchor using the monotonic clock: est = anchor_unix + (now_mono -
//     anchor_mono). This assumes the monotonic source (derived from
//     millis()/esp_timer_get_time() on-device) keeps ticking at its normal
//     rate; only the wall-clock epoch itself goes stale, bounded by the
//     ESP32 crystal's drift (tens of ppm) accumulated since the last sync.
//     That error is negligible against the hour-wide terminal window over
//     the days-to-weeks this fallback is expected to run during stored-boat.
namespace reboot2::time {

class WallClock {
 public:
  static constexpr uint64_t kSyncStaleSecs = 3600;  // NTP considered lost after 1h w/o a sync

  void sync(uint64_t unix_seconds, uint64_t now_mono) {
    anchor_unix_ = unix_seconds;
    anchor_mono_ = now_mono;
    have_anchor_ = true;
    last_sync_mono_ = now_mono;
  }

  bool is_ntp_synced(uint64_t now_mono) const {
    return have_anchor_ && (now_mono - last_sync_mono_) <= kSyncStaleSecs;
  }

  bool has_estimate() const { return have_anchor_; }

  // Best-effort current unix time. Caller should check has_estimate() first
  // — returns 0 if no anchor has ever been set (cold boot, NTP never seen).
  uint64_t estimate(uint64_t now_mono) const {
    if (!have_anchor_) return 0;
    return anchor_unix_ + (now_mono - anchor_mono_);
  }

 private:
  bool have_anchor_ = false;
  uint64_t anchor_unix_ = 0;
  uint64_t anchor_mono_ = 0;
  uint64_t last_sync_mono_ = 0;
};

}  // namespace reboot2::time

#endif  // REBOOT2_WALL_CLOCK_H_
