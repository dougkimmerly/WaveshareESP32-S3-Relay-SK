# Command-loss timer + commit-confirm (fixer ADR 0055 §4)

Job 2 of 4 in the ADR 0055 series. Builds on job 1's v2 "device powered"
NC/NO wire semantics (`src/contact_mapping.h`, already merged). This job
extracts the command-loss timer (CLT) and commit-confirm as pure-C++,
header-only modules with an injected clock, so their logic is covered by
fast host-native tests instead of only being reachable on real hardware.

> **Status:** logic + tests only. `src/main.cpp` `#include`s these headers
> to prove they compile under the ESP32/Arduino toolchain, but does not yet
> wire them into the live relay/SignalK control flow or read
> `REBOOT2_HMAC_SECRET`. That wiring — plus the NTP client, the SignalK log
> entries for terminal-window open/close, and threading real confirm
> requests through — is a follow-up job. The boot fail-safe path (all coils
> LOW at reset → all NC loads powered) is untouched.
>
> **Job 4/4 update:** added the FleetOne window (see below) to
> `command_loss_timer.h` as the same kind of pure-logic, host-tested-only
> addition — still not wired into `main.cpp`. See `docs/HANDOFF.md` for why
> the bench soak checklist requested alongside it wasn't written this job.

## Files

- `src/hmac_sha256.h` — self-contained SHA-256 + HMAC-SHA256 (RFC 4231
  vectors in `test/test_hmac_sha256.cpp`). No mbedtls/Arduino dependency, so
  the exact same bits run in the firmware build and in host tests.
- `src/auth_token.h` — the "home contact" / "confirm" token format: HMAC
  over `context || unix_timestamp`, plus `verify_and_accept()` which enforces
  both freshness (±10 min window) and anti-replay (timestamp must be
  strictly newer than the last one accepted for that context). `context`
  domain-separates token streams (CLT contact vs. each relay's confirm) so a
  captured token from one can't be replayed against another.
- `src/wall_clock.h` — NTP-anchored wall clock with a monotonic-drift
  fallback (see "Time base" below).
- `src/command_loss_timer.h` — the CLT state machine (rungs + terminal
  posture window).
- `src/commit_confirm.h` — the provisional-apply / auto-revert guard for
  WAN-chain power cuts.

## Command-loss timer

Counts elapsed time since the last **authenticated** home-contact token.
Link-layer traffic (WiFi associated, SignalK connected, etc.) and
unauthenticated requests never reset it — only a token that passes
`auth::verify_and_accept()` does. That's enforced by construction:
`CommandLossTimer::process_token()` is the only way to move
`last_contact_mono_` forward, and it always verifies first.

Rungs, all relative to last authenticated contact, each chosen to be safe if
it fires on a false positive (Voyager rule — nothing here can make the
vessel worse off than "no home contact" already implies):

| T+ | Action |
|---|---|
| 12h | Force WAN-chain relays (1-4) to powered-ON |
| 24h | NC-safe 60s power cycle of starlinkInverter + pepRouter; FleetOne window (relay 5) starts, see below |
| 48h | Terminal posture (see below); FleetOne window keeps running unchanged |

A valid token at any point — including from terminal posture — resets to
normal immediately.

**Terminal posture:** WAN chain powered daily 16:00–18:00 UTC, forever, no
further escalation (one long window, not a chatter of short cycles).
Outside the window the WAN chain is powered-OFF, but *only* once terminal
posture has actually been entered — reaching T+48h is itself logged as
entering terminal posture, and every window open/close transition while in
it must be logged to SignalK (both are `Action` values the caller is
expected to log: `kEnterTerminalPosture`, `kTerminalWindowOpen`,
`kTerminalWindowClose`).

### FleetOne window (job 4/4, fixer ADR 0055 §6-7)

A second, independent daily window, distinct from the terminal WAN-chain
window above: once the CLT passes **T+24h** with no authenticated contact
(one rung earlier than terminal posture, at `kRung24h`) it opens
`fleetone_window_open_` at **16:00 UTC** and closes it at **16:30 UTC**,
every day, and keeps doing so **forever, including all through terminal
posture** — escalating from `kRung24h` to `kTerminal` does not interrupt or
resync it. It only stops on a full reset to `Rung::kNormal` (any valid
token), same as the terminal window. This is the intended control point for
relay 5 (fleetOne — NO contact, default de-energized): energize for the
30-minute window, then de-energize. `main.cpp` does not yet act on
`Action::kFleetOneWindowOpen/Close` — see the wiring status banner above,
same as the other CLT actions.

A suspended satellite service just means the window's own connection
attempt fails inside the 30 minutes — harmless by design; Doug enables the
service shore-side once the home dead-man signal shows the boat is dark
(provisioning lag ~1 day, window math verified in fixer ADR 0055 §6-7).

`test/test_command_loss_timer.cpp`'s `test_fleetone_window` covers: no
window before T+24h, open/close at 16:00/16:30 UTC once active, the window
surviving the T+24h→T+48h escalation into terminal posture without being
superseded by the terminal window's own (differently-timed) open/close, and
an immediate close on reset to Normal.

### Time base and drift handling

`CommandLossTimer` takes two independent time inputs:

- a **monotonic** counter (`now_mono`, seconds) for elapsed-since-contact
  and rung thresholds — never resets backward, immune to wall-clock jumps.
- a **wall-clock estimate** (`wall_clock.h`'s `WallClock`) for deciding
  whether "now" falls inside the terminal 16:00–18:00 UTC window.

`WallClock` is anchor + extrapolate:

- `sync(unix_seconds, now_mono)` is called whenever NTP is reachable (WAN
  up). It records `(anchor_unix, anchor_mono)`.
- `estimate(now_mono)` returns `anchor_unix + (now_mono - anchor_mono)` —
  i.e. it always extrapolates from the **last good anchor**, never requires
  a fresh sync to answer.
- `is_ntp_synced(now_mono)` is true only within 1h of the last sync; past
  that the anchor is "stale" (NTP effectively lost, e.g. WAN down during
  terminal posture) but `estimate()` keeps working exactly the same way.

This means the window logic doesn't need an explicit NTP-vs-fallback branch
— `estimate()` is the fallback, always. The only assumption is that the
monotonic clock (`now_mono`, sourced from `millis()`/`esp_timer_get_time()`
on-device) keeps ticking at its true rate; only the wall-clock *epoch*
anchor goes stale, bounded by the ESP32 crystal's drift (tens of ppm)
accumulated since the last sync — negligible against an hour-wide window
over the days-to-weeks this is expected to run unsynced during stored-boat.

If terminal posture is entered before any NTP sync has ever happened (cold
boot with no WAN, straight into 48h with no contact), there is no anchor at
all; the module holds last-known window state (closed) rather than guess,
and picks up normally the moment a sync lands.

`test/test_command_loss_timer.cpp`'s `test_terminal_window_ntp_and_drift`
exercises both a same-day window transition (NTP still "fresh" per
`is_ntp_synced`) and a next-day transition with no further sync at all
(anchor long past `kSyncStaleSecs`), and checks both compute the identical
16:00/18:00 UTC boundaries — proving the fallback needs no special-casing.

## Commit-confirm

Any command that would cut power to a WAN-chain relay (1-4) — i.e. a
power-OFF, not a power-ON, and not a reboot (reboots are already
self-reverting: cut, wait, restore) — applies immediately but provisionally.
`CommitConfirmGuard::tick()` reverts it to the prior powered state if no
authenticated confirm token (same HMAC scheme, scoped to that relay via
`confirm_token_context(relay_id)`) arrives within the 15-minute window.

This is the layer that would have self-healed the 2026-08-07 incident
override: `test/test_commit_confirm.cpp`'s
`test_2026_08_07_incident_self_heals` encodes exactly that shape — an
unconfirmed WAN-relay power cut with no confirm ever arriving — and asserts
the relay is back to powered at T+15min with no human or higher-layer
software involved.

## Secret sourcing (SOPS, no secret in the repo)

The shared HMAC secret is never committed. At bench/deploy time:

1. The secret lives in this homelab's SOPS store (see the `secrets` skill),
   not in this repo.
2. Before a build that needs the CLT/commit-confirm wired live, decrypt it
   into `secrets.local.ini` (gitignored — see `.gitignore` and
   `secrets.local.ini.example` for the exact format), e.g.:
   ```sh
   sops -d path/to/reboot2-hmac-secret.enc.yaml | \
     awk '{print "-D REBOOT2_HMAC_SECRET=\x27\"" $0 "\"\x27"}' \
     > /tmp/flag && printf '[env]\nbuild_flags =\n    %s\n' "$(cat /tmp/flag)" > secrets.local.ini
   ```
   (adapt to however the secret is actually stored in SOPS — the point is
   the decrypted value only ever lands in the gitignored local file, never
   in git history or in `platformio.ini`.)
3. `platformio.ini` has `extra_configs = secrets.local*.ini` (a glob, so
   PlatformIO no-ops when the file is absent — `pio run` stays green on a
   clean checkout with no secret present at all, e.g. in this job's build).
4. Once the follow-up job reads `REBOOT2_HMAC_SECRET` in `main.cpp`, that
   flag becomes load-bearing for a real bench/deploy build; until then it's
   inert.

## Host tests

Same pattern as `test/test_contact_mapping.cpp` — plain g++, no
PlatformIO/Arduino toolchain required:

```sh
for t in hmac_sha256 command_loss_timer commit_confirm; do
  g++ -std=c++17 -Isrc "test/test_${t}.cpp" -o "/tmp/test_${t}" && "/tmp/test_${t}"
done
```
