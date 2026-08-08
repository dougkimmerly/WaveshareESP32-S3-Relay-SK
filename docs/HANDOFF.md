# Handoff / deferred register

Cross-domain and unattended-job discoveries that need a human decision or a
follow-up job, kept here so they don't evaporate. Newest first.

## 2026-08-08 — bench soak checklist (job 4/4) deferred; needs a wiring job first

**Source:** cross-domain request from fixer CC, job 4 of 4 (fixer ADR 0055),
"FleetOne window rung + the bench soak checklist that makes the attended
visit flash-and-verify."

**What shipped from that request:** the FleetOne window rung itself —
`src/command_loss_timer.h` gained `Action::kFleetOneWindowOpen/Close` and a
second, independent daily window (16:00-16:30 UTC, active from T+24h onward,
including forever through terminal posture) alongside the existing terminal
WAN-chain window. Pure logic, host-tested (`test_fleetone_window` in
`test/test_command_loss_timer.cpp`), same "not yet wired into `main.cpp`"
status as everything else `command_loss_timer.h` already does — see
`docs/command-loss-timer.md`.

**What did NOT ship: `docs/bench-checklist.md`.** The requested checklist
assumes things that don't exist in this firmware yet:

- An **accelerated-time build flag** — no such flag exists anywhere in the
  build. There's nothing to document "how to build with."
- The CLT / commit-confirm / FleetOne-window state machines actually
  **running against real GPIO relays** — `main.cpp` only `#include`s these
  headers to prove they compile (job 2's explicit scope decision, held again
  in job 3, and again here). No rung, window, or commit-confirm revert would
  actually happen on a bench board flashed with the current `main.cpp`.
- An **NTP client** feeding `WallClock::sync()` — doesn't exist yet either,
  so there's no wall-clock estimate for the window logic to key off of on
  real hardware.
- `REBOOT2_HMAC_SECRET` being **read** by `main.cpp` — it's threaded through
  the build-flag plumbing (`secrets.local.ini`) but never consumed, so a
  "bench HMAC secret" section would document a secret nothing checks yet.

Writing the checklist as asked would mean documenting bench steps ("walk
each CLT rung, confirm the token-reset, watch the FleetOne window open")
that a person at the bench literally cannot perform on today's firmware —
not "harder than expected," but not physically possible, because the
control-flow wiring the checklist assumes isn't there. That's not a
docs-only deliverable; the prerequisite is a real wiring job: NTP client +
accelerated-time flag design + threading CLT/commit-confirm/FleetOne actions
into the live relay control path in `main.cpp`.

**Why that wiring job isn't something to freelance here:** it touches the
live relay control flow immediately adjacent to the sacred boot fail-safe
path (`digitalWrite(pin, LOW)` at reset → all NC loads powered), on the
firmware that manages the unattended boat's WAN power. It also requires
hardware to verify (Doug's spare ESP32-S3-Relay-6CH — not present in this
job's environment) and at least one real design call (how "accelerated
time" is exposed — a build-time scale factor on the monotonic clock? an
injected fake-RTC? — is an architecture decision, not just plumbing). Repo
guardrails call out exactly this shape: no hardware present, and blast
radius on the WAN-power path, is a park-and-report case rather than a
guess-and-ship one.

**Recommendation:** a dedicated follow-up job — call it job 5 — to (a) add
the accelerated-time build flag, (b) add a minimal NTP client, (c) read
`REBOOT2_HMAC_SECRET` and wire `CommandLossTimer`/`CommitConfirmGuard`
actions (including the new FleetOne window) into `main.cpp`'s real relay
control, all still `pio run`-clean and host-test-covered. **Once that job
lands and its wiring has been sanity-checked, THEN `docs/bench-checklist.md`
can be written truthfully** — reusing the job 1 per-relay semantics table
(README.md's v2-powered table above) as its expected-behavior column, and
covering: boot fail-safe verification first and last (power-cycle → confirm
NC loads ON / NO loads OFF), every CLT rung at accelerated timescale
including token-reset from each rung, commit-confirm revert + confirm,
FleetOne window open/close, and terminal posture across a simulated
midnight. Ask Doug whether the accelerated-time mechanism should be a
compile-time flag (simplest, matches the existing `REBOOT2_*` build-flag
pattern) before that job starts, since it shapes the wiring.

**Not filed as a fixer DB issue** — this job's guardrails don't cover
writing to fixer's Postgres from here; recorded in this repo's own deferred
register instead, per the cross-domain-request instructions.
