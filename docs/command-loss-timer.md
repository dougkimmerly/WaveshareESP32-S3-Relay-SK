# Command-loss timer + commit-confirm (fixer ADR 0055 §4)

Job 2 of 4 in the ADR 0055 series. Builds on job 1's v2 "device powered"
NC/NO wire semantics (`src/contact_mapping.h`, already merged). This job
extracts the command-loss timer (CLT) and commit-confirm as pure-C++,
header-only modules with an injected clock, so their logic is covered by
fast host-native tests instead of only being reachable on real hardware.

> **Status:** wired live as of job 5/4 (fixer ADR 0055 §4 follow-up). See
> "Armed vs. disarmed" and "Live wiring" below for how `src/main.cpp` uses
> these modules. The boot fail-safe path (all coils LOW at reset → all NC
> loads powered) remains untouched — CLT/commit-confirm objects are only
> constructed at the very end of `setup()`, strictly after every relay's
> boot-safe `digitalWrite(pin, LOW)` has already run (see `README.md`'s
> "Boot fail-safe verification" note).
>
> **Job 4/4 update:** added the FleetOne window (see below) to
> `command_loss_timer.h` as the same kind of pure-logic, host-tested-only
> addition. Now wired the same way as the terminal window (job 5/4).

## Armed vs. disarmed

The whole CLT/commit-confirm runtime in `src/main.cpp` is gated on whether
`REBOOT2_HMAC_SECRET` was supplied at build time:

| | Armed (`secrets.local.ini` present) | Disarmed (no secret) |
|---|---|---|
| Relay control | CLT rungs / windows + commit-confirm can drive relays | Fully manual — PUT / `electrical.commands.switch.*` / physical button only, exactly as before this job |
| `electrical.reboot2.clt.armed` | `true` | `false` |
| Other `clt.*` / `confirm.*` SK paths | published every 60s | not created at all |
| `electrical.reboot2.clt.contactToken` / `confirm.relay{1-4}.token` PUT listeners | active | not created |
| `pio run` | green | green |

`electrical.reboot2.clt.armed` is the one path that always exists, in both
modes, republished every 60s (same reconnect rationale as the `semantics`
marker) — that's what makes disarmed mode "loudly visible": home can tell
the CLT is not protecting the boat from SignalK alone, without a bench
connection.

## SignalK paths (job 5/4)

Published every 60s (`clt_confirm_tick()` in `src/main.cpp`), same tick
cadence as the job 3 watchdog paths:

| Path | Type | Meaning |
|---|---|---|
| `electrical.reboot2.clt.armed` | bool | Always published, armed or not — see "Armed vs. disarmed" above. |
| `electrical.reboot2.clt.rung` | string | `"normal"` / `"12h"` / `"24h"` / `"terminal"`. Armed only. |
| `electrical.reboot2.clt.secondsSinceContact` | int | `now_mono - last_contact_mono()`. Armed only. |
| `electrical.reboot2.clt.terminalWindowOpen` | bool | Terminal posture's daily 16:00-18:00 UTC WAN-chain window. Armed only. |
| `electrical.reboot2.clt.fleetoneWindowOpen` | bool | FleetOne's daily 16:00-16:30 UTC window (active from T+24h onward). Armed only. |
| `electrical.reboot2.confirm.pendingCount` | int | How many of relays 1-4 currently have an unconfirmed provisional power-cut. Armed only. |
| `electrical.reboot2.confirm.relay{1-4}.pending` | bool | Per-relay version of the above. Armed only. |

PUT paths (armed only):

| Path | Value | Effect |
|---|---|---|
| `electrical.reboot2.clt.contactToken` | `"<unix_ts>:<64-hex-char HMAC>"` | Authenticated home-contact token; resets the CLT to Normal on success. |
| `electrical.reboot2.confirm.relay{1-4}.token` | `"<unix_ts>:<64-hex-char HMAC>"` | Confirms that relay's pending power-cut, scoped via `confirm_token_context(relay_id)`. |

## Live wiring (job 5/4)

- **Single actuation seam.** Every relay's `SmartSwitchController` is the
  one place all commands funnel through — the PUT listener, the
  `electrical.commands.switch.*` listener, the physical button, a reboot
  pulse, and (when armed) CLT rung/window actions all ultimately call into
  that same controller's `emit()`/`switch_consumer_`. `initialize_relay()`
  attaches one parallel observer to each controller that sees every state
  change from any of those sources uniformly.
- **Commit-confirm interposition.** That observer calls
  `CommitConfirmGuard::submit()` for every state change on relays 1-4.
  Reboot pulses and CLT-driven actions call
  `mark_system_driven_relay_change(relay_id)` immediately before they act,
  which the observer consumes as `is_reboot=true` in the `submit()` call —
  exempting them from tracking, the same way a literal reboot command
  already was. Only externally-sourced commands (SK PUT,
  `electrical.commands.switch.*`) get tracked as provisional and can be
  auto-reverted 15 minutes later by `CommitConfirmGuard::tick()`
  (`clt_confirm_tick()`, every 60s).
- **Authenticated home-contact token.** PUT a
  `"<unix_ts>:<64-hex-char HMAC>"` string (see `src/auth_token.h`'s
  `parse_token()`, host-tested in `test/test_auth_token.cpp`) to
  `electrical.reboot2.clt.contactToken` to reset the CLT. Commit-confirm
  tokens use the same wire format at `electrical.reboot2.confirm.relay{1-4}.token`.
- **Wall clock.** `src/main.cpp` arms the ESP32 SDK's built-in SNTP client
  (`configTime()`) rather than hand-rolling an NTP client — it's the same
  anchor + monotonic-drift model `wall_clock.h` already implements, so
  reusing it is the smallest correct way to give `auth::verify_and_accept`'s
  freshness check a real wall-clock reading. Before the first SNTP sync
  lands, `clt_now_wall()` returns 0, which fails every token's freshness
  check safely closed (no tokens accepted) rather than open. `now_mono` uses
  `esp_timer_get_time()`, not `millis()` — `millis()` wraps every ~49.7
  days, which would corrupt elapsed-since-contact math over the
  weeks-to-months this is meant to run.
- **CLT rung → relay action mapping** (`clt_confirm_tick()` in
  `src/main.cpp`): `kForceWanRelaysOn`/`kTerminalWindowOpen` → relays 1-4
  ON; `kPowerCycleStarlinkAndPep` → `reboot_sequence()` on relays 1 and 3;
  `kTerminalWindowClose` → relays 1-4 OFF; `kFleetOneWindowOpen`/`Close` →
  relay 5 ON/OFF; `kEnterTerminalPosture`/`kExitToNormal` → logged only (no
  direct relay action — the window actions above already carry the actual
  power transitions).
- **Reset mid-CLT is the designed behavior, not a gap.** The CLT is a plain
  heap object with no NVS persistence; an ESP reset re-runs `setup()`,
  which re-derives every relay's state from the boot-safe coil position
  (LOW) before the CLT is even constructed, then calls `g_clt->begin()`
  with the current `now_mono` — i.e. a fresh 12h grace period, with relays
  already in the boot-safe (all-NC-loads-powered) state. Reset can only
  ever return the boat to *more* powered, never less.

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
   `secrets.local.ini.example` for the exact format). The flags go in a
   `[secrets]` section, NOT `[env]` — see the comment above `[secrets]` in
   `platformio.ini` for why (an `[env]` section here would silently drop
   every other build flag instead of adding to them), e.g.:
   ```sh
   sops -d path/to/reboot2-hmac-secret.enc.yaml | \
     awk '{print "-D REBOOT2_HMAC_SECRET=\x27\"" $0 "\"\x27"}' \
     > /tmp/flag && printf '[secrets]\nbuild_flags =\n    %s\n' "$(cat /tmp/flag)" > secrets.local.ini
   ```
   (adapt to however the secret is actually stored in SOPS — the point is
   the decrypted value only ever lands in the gitignored local file, never
   in git history or in `platformio.ini`.)
3. `platformio.ini` has `extra_configs = secrets.local*.ini` (a glob, so
   PlatformIO no-ops when the file is absent — `pio run` stays green on a
   clean checkout with no secret present at all) and interpolates
   `${secrets.build_flags}` into `[env]`'s own `build_flags`, so a present
   `secrets.local.ini` only ever adds the two `-D` flags rather than
   replacing anything.
4. `main.cpp` reads `REBOOT2_HMAC_SECRET` as of job 5/4 — see "Armed vs.
   disarmed" above. `pio run -e pioarduino_esp32s3` is green both with and
   without `secrets.local.ini` present (verified as part of that job).

## Host tests

Same pattern as `test/test_contact_mapping.cpp` — plain g++, no
PlatformIO/Arduino toolchain required:

```sh
for t in hmac_sha256 auth_token command_loss_timer commit_confirm; do
  g++ -std=c++17 -Isrc "test/test_${t}.cpp" -o "/tmp/test_${t}" && "/tmp/test_${t}"
done
```
