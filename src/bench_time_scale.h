#ifndef REBOOT2_BENCH_TIME_SCALE_H_
#define REBOOT2_BENCH_TIME_SCALE_H_

#include <cstdint>

// Accelerated-time build flag (fixer ADR 0055 §4, job 6/6; design decision
// made by the fixer reviewer 2026-08-08): an integer divisor applied to
// every CLT rung duration (command_loss_timer.h), the commit-confirm
// auto-revert window (commit_confirm.h), and the FleetOne/terminal daily
// window schedule -- including the day-length modulus itself, so a
// compressed "day" actually cycles those windows open/closed inside a short
// bench soak instead of once every 24 real hours.
//
// Default 1 = real time; absent from production builds. A bench build
// defines it via `-D BENCH_TIME_SCALE=<n>` (see platformio.ini's
// `env:bench_pioarduino_esp32s3` and docs/bench-checklist.md), e.g. 720
// (12h -> 60s).
//
// Deliberately NOT applied to auth::verify_and_accept()'s freshness/replay
// window (Thresholds::auth_window_secs, CommitConfirmGuard's
// auth_window_secs) -- a token is generated with the real wall-clock time by
// whatever is presenting it (a phone, a script), so scaling that window
// would desync it from the tokens actually being presented rather than
// accelerate anything.
//
// A scaled build must NEVER be flashed to the boat -- see the loud boot log
// + semantics/status SignalK marker in main.cpp, and docs/bench-checklist.md.
#ifndef BENCH_TIME_SCALE
#define BENCH_TIME_SCALE 1
#endif

namespace reboot2::bench {
constexpr uint64_t kTimeScale = BENCH_TIME_SCALE;
}  // namespace reboot2::bench

#endif  // REBOOT2_BENCH_TIME_SCALE_H_
