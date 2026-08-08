# Handoff / deferred register

Cross-domain and unattended-job discoveries that need a human decision or a
follow-up job, kept here so they don't evaporate. Newest first.

## 2026-08-08 — accelerated-time flag + bench-checklist.md written (job 6/6, final)

**Source:** cross-domain request from fixer CC, job 6 (final job in the
fixer ADR 0055 §4 series). The accelerated-time design decision the job 5
entry below flagged as needing Doug was made by the fixer reviewer
(2026-08-08): a compile-time `BENCH_TIME_SCALE` build flag, integer divisor,
default 1.

**What shipped:** `src/bench_time_scale.h` — `BENCH_TIME_SCALE` (default 1,
absent from production builds), applied to `command_loss_timer.h`'s
`Thresholds` (rung durations, terminal/FleetOne window open/close offsets,
and the `day_secs` modulus the window schedules cycle over — not just the
offsets, so a bench "day" actually compresses) and `commit_confirm.h`'s
default `confirm_window_secs`. Deliberately NOT applied to either module's
`auth_window_secs` (token freshness/replay) — those check against the real
wall clock at token-presentation time, so scaling them would desync rather
than accelerate. `src/main.cpp` makes a scaled build unmistakable: boot-time
`ESP_LOGE`, `electrical.reboot2.semantics` becomes
`"v2-powered bench-scale-<n>"`, and a new `electrical.reboot2.benchTimeScale`
SK path always publishes the value. `platformio.ini` gained
`env:bench_pioarduino_esp32s3` (`-D BENCH_TIME_SCALE=720`, 12h→60s) as a
separate, explicitly-named env from the real `env:pioarduino_esp32s3`.

`docs/bench-checklist.md` written per the original (job 4) ask, now
truthful against the actually-wired firmware: bench build config (scale
flag, bench HMAC secret, USB-not-OTA flash), per-relay dummy-load table
reusing job 1's semantics table, boot fail-safe checked first *and* last,
every CLT rung walked at scale including authenticated-token reset from
each rung (incl. terminal), commit-confirm revert + confirm, FleetOne/
terminal window open/close, NTP-lost drift-fallback spot check,
disarmed-mode behavior, pass criteria, and the artifacts list (production
build hash, signed-off checklist, observed semantics marker string, SOPS
secret provisioning steps).

**Validation:** all host tests green, including new
`test/test_bench_time_scale.cpp` run twice (default scale=1, and
`-D BENCH_TIME_SCALE=720`) — proves the divisor actually reaches every
scaled constant and leaves the auth windows alone. `pio run -e
pioarduino_esp32s3` (production, no flag) and `pio run -e
bench_pioarduino_esp32s3` (scale=720) both green.

**Not validated:** the checklist itself has not been run against real
hardware — no spare ESP32-S3-Relay-6CH in this job's environment, same
constraint as every prior job in this series. It's written to be runnable
by someone who didn't write the firmware, but only an actual bench soak
proves that true.

**This closes the ADR 0055 §4 job series (jobs 1-6).** No further follow-up
job identified from this series; any future gap is a fresh ask.

## 2026-08-08 — CLT/commit-confirm wired live (job 5/4); bench checklist is now unblocked

**Source:** cross-domain request from fixer CC, job 5 (fixer ADR 0055 §4
follow-up), asking for the wiring the entry below flagged as a prerequisite.

**What shipped:** `src/main.cpp` now instantiates
`reboot2::clt::CommandLossTimer` + `reboot2::confirm::CommitConfirmGuard`
live, gated on `REBOOT2_HMAC_SECRET`; a single actuation seam (each relay's
`SmartSwitchController`) carries PUT, `electrical.commands.switch.*`,
button, reboot, and CLT-rung commands alike; commit-confirm interposes on
externally-sourced WAN-chain (1-4) power-cuts only (reboots and CLT-driven
actions are exempt, matching the module's existing `is_reboot` semantics);
an authenticated home-contact token PUT path
(`electrical.reboot2.clt.contactToken`) and per-relay confirm token paths
reset/confirm the timer; CLT/commit-confirm state publishes to
`electrical.reboot2.clt.*`/`confirm.*`; the ESP32 SDK's built-in SNTP client
(`configTime()`) feeds the wall clock instead of a hand-rolled NTP client.
Full detail in `docs/command-loss-timer.md` ("Armed vs. disarmed", "Live
wiring", "SignalK paths"). Boot fail-safe verified untouched — CLT/confirm
objects are constructed strictly after all 6 relays' boot-safe
`digitalWrite(pin, LOW)` calls (see `README.md`). `pio run
-e pioarduino_esp32s3` green both with and without `secrets.local.ini`
(this job also fixed a latent bug in that plumbing — see below). Host tests
green, including a new `test/test_auth_token.cpp` for the token wire-format
parser and a `last_contact_mono()` getter test in
`test/test_command_loss_timer.cpp`.

**Deviation from the job text — no accelerated-time build flag.** The job
asked for the wiring but not the accelerated-time mechanism the entry below
also called out as a bench-checklist prerequisite; that's still a genuine
design decision (compile-time scale factor on the monotonic clock vs. an
injected fake-RTC) belonging to Doug, not something to freelance. Not
needed for `pio run`/host-test acceptance, only for a bench soak walking
every CLT rung in real time — see the recommendation below.

**Also fixed in this job:** `secrets.local.ini`'s `[env]` section was
silently REPLACING (not merging with) `platformio.ini`'s own `[env]
build_flags` — a real, previously-unexercised bug (job 4's entry below notes
nothing had ever consumed `REBOOT2_HMAC_SECRET`, so a populated
`secrets.local.ini` had never actually been build-tested). `pio run` with a
real secret present failed outright with SensESP's `#error` on missing
`CORE_DEBUG_LEVEL`. Fixed by moving the two secret flags to their own
`[secrets]` section, interpolated additively into `[env]` — see
`platformio.ini` and `secrets.local.ini.example`.

**Recommendation:** `docs/bench-checklist.md` (job 4's ask) can now be
written truthfully — the wiring it depends on has landed. It still needs
Doug's decision on the accelerated-time mechanism (see above) before a
useful bench walkthrough script can be written, and needs the actual bench
hardware (spare ESP32-S3-Relay-6CH) to verify against, not just to write
words describing steps.

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
