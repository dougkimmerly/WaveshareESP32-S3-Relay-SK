// Host-native test for src/command_loss_timer.h with an accelerated fake
// clock (no real sleeping — we just advance integer "seconds" counters).
//
//   g++ -std=c++17 -Isrc test/test_command_loss_timer.cpp -o /tmp/t && /tmp/t

#include <algorithm>
#include <cassert>
#include <cstdio>

#include "auth_token.h"
#include "command_loss_timer.h"

using namespace reboot2::clt;
using reboot2::auth::make_token;

const std::string kSecret = "bench-secret";

bool contains(const std::vector<Action>& actions, Action a) {
  return std::find(actions.begin(), actions.end(), a) != actions.end();
}

// --- Case 1: unauthenticated/replayed/expired tokens never reset the CLT ---
void test_bad_tokens_never_reset() {
  CommandLossTimer clt(kSecret);
  clt.begin(/*now_mono=*/0);

  // Advance close to the 12h rung so a false reset would be visible.
  uint64_t now_mono = 11 * 3600;

  // Forged token (wrong secret) -> rejected.
  auto forged = make_token("reboot2.clt.contact", now_mono, "wrong-secret");
  assert(!clt.process_token(forged, now_mono, now_mono));

  // Expired token (11 minutes old, outside +-10 min window) -> rejected.
  auto expired = make_token("reboot2.clt.contact", now_mono - 11 * 60, kSecret);
  assert(!clt.process_token(expired, now_mono, now_mono));

  // Valid token accepted once...
  auto valid = make_token("reboot2.clt.contact", now_mono, kSecret);
  assert(clt.process_token(valid, now_mono, now_mono));

  // ...but is a repeat/replay on a second presentation.
  assert(!clt.process_token(valid, now_mono, now_mono));

  std::printf("  test_bad_tokens_never_reset: ok\n");
}

// --- Case 2: rungs fire at threshold; a valid token resets from any rung ---
void test_rungs_fire_and_reset() {
  {
    CommandLossTimer clt(kSecret);
    clt.begin(0);

    // Just before 12h: still normal, no action.
    auto actions = clt.tick(12 * 3600 - 1);
    assert(actions.empty());
    assert(clt.rung() == Rung::kNormal);

    // At 12h: force WAN relays on.
    actions = clt.tick(12 * 3600);
    assert(contains(actions, Action::kForceWanRelaysOn));
    assert(clt.rung() == Rung::kRung12h);

    // At 24h: NC-safe power cycle.
    actions = clt.tick(24 * 3600);
    assert(contains(actions, Action::kPowerCycleStarlinkAndPep));
    assert(clt.rung() == Rung::kRung24h);

    // At 48h: terminal posture.
    actions = clt.tick(48 * 3600);
    assert(contains(actions, Action::kEnterTerminalPosture));
    assert(clt.rung() == Rung::kTerminal);

    // A valid token resets to normal from terminal.
    auto token = make_token("reboot2.clt.contact", 48 * 3600 + 10, kSecret);
    assert(clt.process_token(token, 48 * 3600 + 10, 48 * 3600 + 10));
    actions = clt.tick(48 * 3600 + 10);
    assert(contains(actions, Action::kExitToNormal));
    assert(clt.rung() == Rung::kNormal);
  }

  // Reset from rung 12h and rung 24h too, not just terminal.
  {
    CommandLossTimer clt(kSecret);
    clt.begin(0);
    clt.tick(13 * 3600);
    assert(clt.rung() == Rung::kRung12h);
    auto token = make_token("reboot2.clt.contact", 13 * 3600, kSecret);
    assert(clt.process_token(token, 13 * 3600, 13 * 3600));
    auto actions = clt.tick(13 * 3600);
    assert(contains(actions, Action::kExitToNormal));
    assert(clt.rung() == Rung::kNormal);
  }
  {
    CommandLossTimer clt(kSecret);
    clt.begin(0);
    clt.tick(25 * 3600);
    assert(clt.rung() == Rung::kRung24h);
    auto token = make_token("reboot2.clt.contact", 25 * 3600, kSecret);
    assert(clt.process_token(token, 25 * 3600, 25 * 3600));
    auto actions = clt.tick(25 * 3600);
    assert(contains(actions, Action::kExitToNormal));
    assert(clt.rung() == Rung::kNormal);
  }

  std::printf("  test_rungs_fire_and_reset: ok\n");
}

// --- Case 3: terminal window opens/closes at the right UTC times, both
// under NTP and under millis-based drift fallback ---
void test_terminal_window_ntp_and_drift() {
  // Anchor unix time to a fixed synthetic epoch: pick now_unix such that
  // now_unix % 86400 == 0 (UTC midnight) for a clean reference.
  const uint64_t kMidnightUnix = 1'700'000'000ULL - (1'700'000'000ULL % 86400);

  CommandLossTimer clt(kSecret);
  clt.begin(0);
  clt.sync_wall_clock(kMidnightUnix, /*now_mono=*/0);

  // Drive into terminal posture.
  clt.tick(48 * 3600);
  assert(clt.rung() == Rung::kTerminal);
  assert(!clt.terminal_window_open());

  // 15:59 UTC on the terminal day: still closed.
  uint64_t mono_at_1559 = 48 * 3600 + (15 * 3600 + 59 * 60);
  auto actions = clt.tick(mono_at_1559);
  assert(!clt.terminal_window_open());
  assert(!contains(actions, Action::kTerminalWindowOpen));

  // 16:00 UTC: window opens (still NTP-synced here, sync was only 1 tick ago
  // relative to kSyncStaleSecs).
  uint64_t mono_at_1600 = 48 * 3600 + 16 * 3600;
  actions = clt.tick(mono_at_1600);
  assert(clt.terminal_window_open());
  assert(contains(actions, Action::kTerminalWindowOpen));

  // 18:00 UTC: window closes.
  uint64_t mono_at_1800 = 48 * 3600 + 18 * 3600;
  actions = clt.tick(mono_at_1800);
  assert(!clt.terminal_window_open());
  assert(contains(actions, Action::kTerminalWindowClose));

  // --- Now the NTP-lost -> drift-fallback path. ---
  // Fresh CLT, sync once, then let the sync go stale (no further syncs) and
  // keep advancing the monotonic clock — the window must still open/close
  // at the right wall-clock times because the drift-fallback estimate
  // extrapolates linearly from the last good anchor.
  CommandLossTimer clt2(kSecret);
  clt2.begin(0);
  clt2.sync_wall_clock(kMidnightUnix, 0);
  clt2.tick(48 * 3600);  // enter terminal

  // Advance far enough that the anchor is stale per WallClock's
  // kSyncStaleSecs (1h), well past 15:59 the next "day" in monotonic terms —
  // use day 2 to prove the fallback survives across a full day with no NTP.
  uint64_t day2_1559 = 48 * 3600 + 24 * 3600 + (15 * 3600 + 59 * 60);
  actions = clt2.tick(day2_1559);
  assert(!clt2.terminal_window_open());

  uint64_t day2_1600 = 48 * 3600 + 24 * 3600 + 16 * 3600;
  actions = clt2.tick(day2_1600);
  assert(clt2.terminal_window_open());
  assert(contains(actions, Action::kTerminalWindowOpen));

  uint64_t day2_1800 = 48 * 3600 + 24 * 3600 + 18 * 3600;
  actions = clt2.tick(day2_1800);
  assert(!clt2.terminal_window_open());
  assert(contains(actions, Action::kTerminalWindowClose));

  // Sanity: without ever syncing wall clock, terminal posture holds
  // (no window actions at all, since there's no time base to judge against).
  CommandLossTimer clt3(kSecret);
  clt3.begin(0);
  actions = clt3.tick(48 * 3600);
  assert(clt3.rung() == Rung::kTerminal);
  assert(!clt3.terminal_window_open());
  actions = clt3.tick(48 * 3600 + 16 * 3600);
  assert(!clt3.terminal_window_open());  // never opens: no wall-clock estimate ever set

  std::printf("  test_terminal_window_ntp_and_drift: ok\n");
}

// --- Case 4: fleetOne window (fixer ADR 0055 §6-7) opens/closes at
// 16:00-16:30 UTC starting at T+24h (one rung earlier than terminal
// posture's own window), and keeps firing daily straight through terminal
// posture without being superseded by it. ---
void test_fleetone_window() {
  const uint64_t kMidnightUnix = 1'700'000'000ULL - (1'700'000'000ULL % 86400);

  // Not yet active before T+24h: at T+12h, 16:00 UTC produces no window.
  {
    CommandLossTimer clt(kSecret);
    clt.begin(0);
    clt.sync_wall_clock(kMidnightUnix, 0);
    clt.tick(12 * 3600);
    assert(clt.rung() == Rung::kRung12h);
    auto actions = clt.tick(16 * 3600);
    assert(!clt.fleetone_window_open());
    assert(!contains(actions, Action::kFleetOneWindowOpen));
  }

  // Active from T+24h: opens at 16:00 UTC, closes at 16:30 UTC (not 18:00 —
  // that's the terminal window's own, separate close time).
  {
    CommandLossTimer clt(kSecret);
    clt.begin(0);
    clt.sync_wall_clock(kMidnightUnix, 0);
    clt.tick(24 * 3600);
    assert(clt.rung() == Rung::kRung24h);
    assert(!clt.fleetone_window_open());

    auto actions = clt.tick(24 * 3600 + 15 * 3600 + 59 * 60);  // 15:59 UTC
    assert(!clt.fleetone_window_open());
    assert(!contains(actions, Action::kFleetOneWindowOpen));

    actions = clt.tick(24 * 3600 + 16 * 3600);  // 16:00 UTC
    assert(clt.fleetone_window_open());
    assert(contains(actions, Action::kFleetOneWindowOpen));

    actions = clt.tick(24 * 3600 + 16 * 3600 + 29 * 60);  // 16:29 UTC: still open
    assert(clt.fleetone_window_open());

    actions = clt.tick(24 * 3600 + 16 * 3600 + 30 * 60);  // 16:30 UTC: closes
    assert(!clt.fleetone_window_open());
    assert(contains(actions, Action::kFleetOneWindowClose));
  }

  // Escalating from kRung24h into kTerminal does NOT close the fleetOne
  // window early or skip a day — it keeps running through terminal posture,
  // in addition to the (differently-timed) terminal WAN-chain window.
  {
    CommandLossTimer clt(kSecret);
    clt.begin(0);
    clt.sync_wall_clock(kMidnightUnix, 0);
    clt.tick(24 * 3600 + 16 * 3600);  // T+24h, day 1's 16:00 UTC: window opens
    assert(clt.fleetone_window_open());

    // Cross into terminal posture (T+48h) while still inside the window
    // (day 2's 16:00-16:30 UTC slot) — window stays open, and terminal
    // posture's own entry/window logic also fires independently.
    auto actions = clt.tick(48 * 3600 + 16 * 3600 + 10 * 60);
    assert(clt.rung() == Rung::kTerminal);
    assert(contains(actions, Action::kEnterTerminalPosture));
    assert(clt.fleetone_window_open());
    assert(clt.terminal_window_open());

    // Both windows close at their own, independent times: fleetOne at
    // 16:30 UTC, terminal's WAN-chain window not until 18:00 UTC.
    actions = clt.tick(48 * 3600 + 16 * 3600 + 30 * 60);
    assert(!clt.fleetone_window_open());
    assert(contains(actions, Action::kFleetOneWindowClose));
    assert(clt.terminal_window_open());  // unaffected — still open until 18:00

    actions = clt.tick(48 * 3600 + 18 * 3600);
    assert(!clt.terminal_window_open());
    assert(contains(actions, Action::kTerminalWindowClose));
  }

  // A valid token reset to Normal closes an open fleetOne window
  // immediately, same as it does the terminal window.
  {
    CommandLossTimer clt(kSecret);
    clt.begin(0);
    clt.sync_wall_clock(kMidnightUnix, 0);
    clt.tick(24 * 3600 + 16 * 3600 + 10 * 60);
    assert(clt.fleetone_window_open());

    uint64_t now = 24 * 3600 + 16 * 3600 + 10 * 60;
    auto token = make_token("reboot2.clt.contact", now, kSecret);
    assert(clt.process_token(token, now, now));
    auto actions = clt.tick(now);
    assert(contains(actions, Action::kExitToNormal));
    assert(contains(actions, Action::kFleetOneWindowClose));
    assert(!clt.fleetone_window_open());
    assert(clt.rung() == Rung::kNormal);
  }

  std::printf("  test_fleetone_window: ok\n");
}

int main() {
  test_bad_tokens_never_reset();
  test_rungs_fire_and_reset();
  test_terminal_window_ntp_and_drift();
  test_fleetone_window();
  std::printf("test_command_loss_timer: all assertions passed\n");
  return 0;
}
