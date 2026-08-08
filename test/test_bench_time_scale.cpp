// Host-native test for src/bench_time_scale.h -- proves BENCH_TIME_SCALE
// actually compresses CLT rungs, the commit-confirm window, and the daily
// window schedule (including the day-length modulus) by the expected
// factor, and that the auth freshness windows are left unscaled.
//
// Unlike the other host tests this one MUST be compiled with the flag
// under test defined, e.g. BENCH_TIME_SCALE=720 (the same value
// platformio.ini's env:bench_pioarduino_esp32s3 uses):
//
//   g++ -std=c++17 -Isrc -D BENCH_TIME_SCALE=720 \
//     test/test_bench_time_scale.cpp -o /tmp/t && /tmp/t
//
// A second run at the default (no -D, i.e. scale=1) confirms real-time
// behavior is exactly the unscaled behavior already covered by
// test_command_loss_timer.cpp / test_commit_confirm.cpp.

#include <algorithm>
#include <cassert>
#include <cstdio>

#include "auth_token.h"
#include "bench_time_scale.h"
#include "command_loss_timer.h"
#include "commit_confirm.h"

using namespace reboot2::clt;
using reboot2::auth::make_token;

const std::string kSecret = "bench-secret";

bool contains(const std::vector<Action>& actions, Action a) {
  return std::find(actions.begin(), actions.end(), a) != actions.end();
}

// CLT rung thresholds land at real-duration / BENCH_TIME_SCALE, not at the
// unscaled real-time value.
void test_clt_rungs_scale() {
  CommandLossTimer clt(kSecret);
  clt.begin(0);

  uint64_t scaled_12h = (12 * 3600) / reboot2::bench::kTimeScale;

  // Just before the scaled 12h threshold: still normal.
  auto actions = clt.tick(scaled_12h - 1);
  assert(clt.rung() == Rung::kNormal);

  // At the scaled threshold: rung fires.
  actions = clt.tick(scaled_12h);
  assert(contains(actions, Action::kForceWanRelaysOn));
  assert(clt.rung() == Rung::kRung12h);

  std::printf("  test_clt_rungs_scale: ok (scale=%llu, 12h threshold=%llus)\n",
              static_cast<unsigned long long>(reboot2::bench::kTimeScale),
              static_cast<unsigned long long>(scaled_12h));
}

// The daily window schedule (terminal + FleetOne) cycles over a compressed
// day (day_secs = 86400 / scale) and opens/closes at the correspondingly
// compressed open/close offsets -- proving a bench soak can actually
// observe multiple window cycles without waiting real days.
void test_window_schedule_scale() {
  CommandLossTimer clt(kSecret);
  clt.begin(0);
  // Anchor wall clock to a synthetic "midnight" in the *compressed* frame:
  // any unix time whose value modulo the compressed day is 0.
  Thresholds th;
  uint64_t day = th.day_secs;
  const uint64_t kMidnightUnix = 1'700'000'000ULL - (1'700'000'000ULL % day);
  clt.sync_wall_clock(kMidnightUnix, 0);

  uint64_t scaled_48h = (48 * 3600) / reboot2::bench::kTimeScale;
  clt.tick(scaled_48h);
  assert(clt.rung() == Rung::kTerminal);

  uint64_t scaled_open = th.window_open_utc_secs;
  uint64_t scaled_close = th.window_close_utc_secs;
  assert(scaled_open < day);
  assert(scaled_close < day);

  auto actions = clt.tick(scaled_48h + scaled_open);
  assert(clt.terminal_window_open());
  assert(contains(actions, Action::kTerminalWindowOpen));

  actions = clt.tick(scaled_48h + scaled_close);
  assert(!clt.terminal_window_open());
  assert(contains(actions, Action::kTerminalWindowClose));

  std::printf("  test_window_schedule_scale: ok (day=%llus, open=%llus, close=%llus)\n",
              static_cast<unsigned long long>(day),
              static_cast<unsigned long long>(scaled_open),
              static_cast<unsigned long long>(scaled_close));
}

// The commit-confirm auto-revert window scales too.
void test_commit_confirm_window_scale() {
  reboot2::confirm::CommitConfirmGuard guard(kSecret);
  uint64_t scaled_window = (15 * 60) / reboot2::bench::kTimeScale;

  auto disp = guard.submit(1, false, true, false, 0);
  assert(disp == reboot2::confirm::Disposition::kAppliedProvisional);

  if (scaled_window > 0) {
    auto reverts = guard.tick(scaled_window - 1);
    assert(reverts.empty());
  }
  auto reverts = guard.tick(scaled_window);
  assert(reverts.size() == 1);
  assert(reverts[0].relay_id == 1);

  std::printf("  test_commit_confirm_window_scale: ok (window=%llus)\n",
              static_cast<unsigned long long>(scaled_window));
}

// auth_window_secs (token freshness/replay) is deliberately NOT scaled --
// a token's timestamp is real wall-clock time set by whatever presented it.
void test_auth_window_not_scaled() {
  Thresholds th;
  assert(th.auth_window_secs == 600);

  reboot2::confirm::CommitConfirmGuard guard(kSecret, /*confirm_window_secs=*/1,
                                              /*auth_window_secs=*/600);
  guard.submit(2, false, true, false, 0);
  // A confirm token stamped 601s in the "past" relative to now_wall=601
  // (i.e. at wall time 0) is outside the unscaled 600s freshness window,
  // regardless of BENCH_TIME_SCALE.
  auto stale = make_token(reboot2::confirm::confirm_token_context(2), /*ts=*/0, kSecret);
  assert(!guard.confirm(2, stale, /*now_wall=*/601));

  std::printf("  test_auth_window_not_scaled: ok\n");
}

int main() {
  test_clt_rungs_scale();
  test_window_schedule_scale();
  test_commit_confirm_window_scale();
  test_auth_window_not_scaled();
  std::printf("test_bench_time_scale: all assertions passed (BENCH_TIME_SCALE=%d)\n",
              BENCH_TIME_SCALE);
  return 0;
}
