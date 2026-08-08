# Bench soak checklist (fixer ADR 0055 §4, job 6/6)

Attended-only. Run this on Doug's spare ESP32-S3-Relay-6CH before any bench
soak that's meant to actually exercise the command-loss timer (CLT),
commit-confirm, and FleetOne/terminal window logic wired live in
`src/main.cpp` (job 5/4) at a timescale a person can sit through — see
`docs/command-loss-timer.md` for what each layer does and why.

**This is a person-in-the-loop procedure, not a job for an unattended batch
run.** It requires physical hardware, a USB cable, and someone watching LEDs
and a SignalK client in real time.

---

## ⚠️ Accelerated time — read this first

A bench soak uses a **compile-time build flag**, `BENCH_TIME_SCALE`
(`src/bench_time_scale.h`), that divides every CLT rung duration, the
commit-confirm auto-revert window, and the FleetOne/terminal daily window
schedule (including the day-length itself) by an integer factor — e.g. `720`
turns the 12h/24h/48h rungs into 60s/120s/240s and compresses the 24h
window-schedule "day" into 120s. It does **not** touch the token
freshness/replay window (`auth_window_secs`) — a home-contact or confirm
token is always checked against the real wall clock at the moment it's
presented, scaled or not.

**A `BENCH_TIME_SCALE` build must NEVER be flashed to the boat.** It makes
the WAN-chain relays cycle on a schedule of minutes instead of days —
harmless on a bench with dummy loads, actively wrong on Distant Shores II.
Guardrails against flashing the wrong build by mistake:

- It is loudly self-identifying: `electrical.reboot2.semantics` reads
  `"v2-powered bench-scale-<N>"` instead of plain `"v2-powered"`, a
  dedicated `electrical.reboot2.benchTimeScale` (int) path publishes `<N>`
  (always `1` on a production build), and the boot log prints
  `*** BENCH_TIME_SCALE=<N> — this is an ACCELERATED-TIME BENCH BUILD. DO
  NOT FLASH TO THE BOAT. ***` in bright red (`ESP_LOGE`).
- The PlatformIO env is separate and explicitly named:
  `env:bench_pioarduino_esp32s3` (`platformio.ini`), vs. the real
  `env:pioarduino_esp32s3` used for every production/OTA build.
- **This checklist records the production build hash separately from the
  bench build** (§6) — the artifact that actually goes on the boat is never
  the one built for this soak.

---

## 1. Bench build config

1. Copy `secrets.local.ini.example` to `secrets.local.ini` (gitignored) and
   fill in a **bench** HMAC secret — this does NOT need to be the real
   production `REBOOT2_HMAC_SECRET` from SOPS; a throwaway value is fine and
   arguably safer for a bench that may sit on an open bench network. Leave
   `REBOOT2_OTA_PASSWORD` as the template default or set a bench-only value
   — OTA is not used for this soak (see step 3).
2. Build the bench-scale image:
   ```sh
   pio run -e bench_pioarduino_esp32s3
   ```
   This uses `-D BENCH_TIME_SCALE=720` (12h → 60s) — see `platformio.ini`.
   Confirm the build succeeds and note it is **not** the same env as the
   production build (`env:pioarduino_esp32s3`, no `BENCH_TIME_SCALE` flag).
3. **Flash via USB, not OTA.** This device has no prior firmware to OTA
   from, and OTA use here would risk exercising the same upload path used
   for the boat with a bench-only password:
   ```sh
   pio run -e bench_pioarduino_esp32s3 -t upload --upload-port /dev/tty.<bench-board-port>
   ```
4. Connect the board to a SignalK server reachable from the bench (a local
   `signalk-server` instance is easiest) and to a SignalK client (e.g.
   SKipper, or `curl`/`websocat` against the SK HTTP/WS API) to watch the
   `electrical.reboot2.*` paths below live.

## 2. Dummy loads / LED indicators per relay

Wire a dummy load (an LED + resistor, or a 12V test bulb) to each relay's
output so "powered" vs. "unpowered" is visible without a meter. Expected
behavior per relay — this is job 1's v2 "device powered" semantics table
(`README.md`), reused here as the column to check bench observations
against:

| Relay | Name | Contact | `state`=`true` means | Boot state (coil LOW) |
|---|---|---|---|---|
| 1 | starlinkInverter | NC | device powered | powered |
| 2 | cellModem | NC | device powered | powered |
| 3 | pepRouter | NC | device powered | powered |
| 4 | dataHub | NC | device powered | powered |
| 5 | fleetOne | NO | device powered | unpowered |
| 6 | relay6 (spare) | NO | device powered | unpowered |

For NC relays (1-4), the dummy load should be wired to the NC contact so it
reads "on" when the coil is de-energized (matches "boot = powered"). For NO
relays (5/6), wire to the NO contact so it reads "on" only when the coil is
energized.

## 3. Boot fail-safe — first and last

This is the sacred invariant (`README.md`, repo guardrails): at power-on or
brownout, every coil is driven LOW before any firmware logic runs, which
means all 4 NC loads are powered and both NO loads are unpowered,
independent of WiFi/SignalK/CLT state. **Any deviation from this fails the
bench, full stop — do not continue the soak.**

Check it **both before and after** the soak:

1. **Before:** power-cycle the bench board (unplug/replug, or hardware
   reset). Confirm via the dummy loads (not SignalK — SK may not have
   reconnected yet):
   - Relays 1-4 (NC): **loads ON**.
   - Relays 5-6 (NO): **loads OFF**.
2. **After:** once the full soak (§4) is complete, power-cycle again and
   confirm the identical result — regardless of what rung/window state the
   CLT was in immediately before the cycle. (`docs/command-loss-timer.md`
   "Reset mid-CLT is the designed behavior, not a gap" — a reset always
   re-derives from the boot-safe coil position first, then gives the CLT a
   fresh grace period.)

## 4. Walk every CLT rung at scale

With `BENCH_TIME_SCALE=720`, 12h/24h/48h become 60s/120s/240s. Watch
`electrical.reboot2.clt.rung` and `electrical.reboot2.clt.armed` (must read
`true` — if `false`, the bench secret didn't build in; check step 1).

1. **T+0 (boot):** `clt.rung` = `"normal"`. Do not send any contact token
   yet.
2. **T+60s (scaled 12h):** `clt.rung` → `"12h"`; relays 1-4 all report
   `state`=`true` (forced ON) even if a dummy "outage" had one of them off.
3. **Token reset from rung 12h:** PUT an authenticated home-contact token
   (`"<unix_ts>:<64-hex HMAC>"`, HMAC'd with the bench secret — see
   `src/auth_token.h` / `docs/command-loss-timer.md` for the wire format,
   `test/test_auth_token.cpp` for a worked example) to
   `electrical.reboot2.clt.contactToken`. Confirm `clt.rung` returns to
   `"normal"` and `clt.secondsSinceContact` resets near 0.
4. **T+120s (scaled 24h):** let it run past 12h again (no token this time).
   Confirm `clt.rung` → `"24h"`, and watch `starlinkInverter` / `pepRouter`
   dummy loads for the NC-safe 60s power cycle (off, then back on ~60s
   later — this is real-world seconds, not scaled, since `reboot_sequence`'s
   `ms` parameter is a hardware timing constant, not a CLT duration).
5. **FleetOne window (starts at T+120s, i.e. scaled 24h):** watch
   `electrical.reboot2.clt.fleetoneWindowOpen` and relay 5's dummy load —
   it should open/close on the compressed daily cycle (`day_secs` = 120s at
   this scale, window open/close offsets scaled the same way — see
   `src/bench_time_scale.h`). You should see it cycle open/closed at least
   twice inside a few minutes of observation.
6. **Token reset from rung 24h:** same as step 3, confirm reset to
   `"normal"` also closes an open FleetOne window immediately
   (`test_fleetone_window`'s reset case, `test/test_command_loss_timer.cpp`).
7. **T+240s (scaled 48h): terminal posture.** Confirm `clt.rung` →
   `"terminal"`. Watch `electrical.reboot2.clt.terminalWindowOpen` cycle on
   the compressed terminal window schedule (open/close offsets also scaled)
   — relays 1-4 should power ON when the window opens and OFF when it
   closes, and the FleetOne window (§5) should keep running unaffected
   alongside it.
8. **Authenticated token reset from terminal:** confirm a valid contact
   token resets `clt.rung` to `"normal"` from terminal posture too, closing
   both windows if either was open (`test_rungs_fire_and_reset`,
   `test_fleetone_window`).

## 5. Commit-confirm revert + confirm

With the CLT back at `"normal"` (§4 step 8):

1. Send a command that cuts power to a WAN-chain relay (1-4) — a SignalK PUT
   to `electrical.<relay>.state` = `false`, or `electrical.commands.switch.*`
   = `false`. Confirm the dummy load goes off immediately (applies
   provisionally) and `electrical.reboot2.confirm.relay<N>.pending` reads
   `true`.
2. **Without confirming**, wait for `(15 * 60) / BENCH_TIME_SCALE` seconds
   (1s at scale 720 — round up to a couple of seconds to be safe against
   tick jitter, since `clt_confirm_tick()` only runs once per real 60s in
   `main.cpp`; at `BENCH_TIME_SCALE=720` the window is shorter than the
   tick interval, so expect the revert on the *next* tick after the
   deadline, not instantly — see note below). Confirm the relay auto-reverts
   to powered and `confirm.relay<N>.pending` clears.
3. Repeat the cut, but this time PUT an authenticated confirm token (scoped
   to that relay via `confirm_token_context`) to
   `electrical.reboot2.confirm.relay<N>.token` before the deadline. Confirm
   the relay stays off (sticks) and `pending` clears immediately on the
   confirm, not at the deadline.

> **Known bench-scale wrinkle:** `clt_confirm_tick()` in `main.cpp` runs on
> a fixed real-world 60s `event_loop()->onRepeat()`, which is **not** itself
> divided by `BENCH_TIME_SCALE` — only the *thresholds it compares against*
> are. At very aggressive scales (e.g. 720) the commit-confirm window
> (900s/720 ≈ 1.25s) can be shorter than the 60s tick, so a revert that's
> "due" fires on the next tick rather than the instant the deadline passes.
> This is a real property of the current wiring, not a bug in the scale
> math — note it if the bench observation says "revert happened ~60s late"
> rather than treating that as a failure.

## 6. NTP-lost drift-fallback spot check

1. With the bench board on a network with no NTP reachability (block
   `pool.ntp.org` / `time.nist.gov`, or simply don't give it internet
   access), confirm `clt.terminalWindowOpen` / `clt.fleetoneWindowOpen`
   **never open** even after reaching terminal posture or T+24h — no
   wall-clock estimate exists yet, so the module correctly holds "closed"
   rather than guessing (`docs/command-loss-timer.md` "If terminal posture
   is entered before any NTP sync...").
2. Restore network access and confirm `configTime()`'s SNTP sync lands
   (watch for the ESP-IDF SNTP log line, or simply that the window logic
   starts working on the next tick) and the window schedule picks up
   correctly from that point on.
3. This step is a spot check, not a full drift test — `wall_clock.h`'s
   anchor + monotonic-extrapolation behavior across a genuinely stale
   (>1h-old) sync is already covered by
   `test_terminal_window_ntp_and_drift` in
   `test/test_command_loss_timer.cpp`; the bench only needs to confirm the
   real SNTP client actually feeds `WallClock::sync()` end to end.

## Pass criteria

The bench soak passes if and only if:

- §3's boot fail-safe check is identical before and after the soak.
- Every rung transition in §4 fires within one bench tick (60s real time)
  of its scaled threshold, and every rung is reachable both by letting time
  pass and by an authenticated token resetting out of it.
- The FleetOne and terminal windows each open and close at least once on
  their compressed schedule, and the FleetOne window is observed surviving
  the 24h→48h escalation without being closed by the terminal window's
  entry (matches `test_fleetone_window`).
- Commit-confirm both auto-reverts (unconfirmed) and sticks (confirmed) as
  in §5.
- §6's NTP-lost / NTP-restored behavior matches (windows never open with no
  wall-clock estimate; window logic works normally once synced).
- `electrical.reboot2.clt.armed` read `true` throughout (bench secret was
  actually present — a soak run disarmed proves nothing).
- The build used for the soak was `env:bench_pioarduino_esp32s3` and never
  touched an OTA path or `192.168.22.x`.

Any deviation fails the bench — do not proceed to a production flash until
the cause is understood and, if it's a firmware bug, fixed and re-soaked.

## Artifacts for the attended visit

Record and keep with the soak results:

1. **Production build hash** — the actual bits going on the boat, built and
   hashed separately from (and never using) the bench env:
   ```sh
   pio run -e pioarduino_esp32s3
   sha256sum .pio/build/pioarduino_esp32s3/firmware.bin
   ```
   This must be a plain `env:pioarduino_esp32s3` build with no
   `BENCH_TIME_SCALE` flag — verify `electrical.reboot2.benchTimeScale`
   reads `1` (or the path is simply absent pre-boot; check the boot log has
   no `ACCELERATED-TIME BENCH BUILD` warning) before flashing to the boat.
2. **This checklist, signed off** — date, who ran it, which steps passed,
   any deviations and their resolution.
3. **Semantics marker string observed during the soak** —
   `electrical.reboot2.semantics` should have read
   `"v2-powered bench-scale-720"` (or whatever scale was used) throughout;
   record the exact string as evidence the bench build was correctly
   self-identifying and was not mistaken for production.
4. **SOPS secret provisioning steps actually used** — which secret(s) were
   decrypted (bench-only HMAC secret vs. the real
   `REBOOT2_HMAC_SECRET`/`REBOOT2_OTA_PASSWORD` for the eventual production
   build), confirming the production build's secrets came from SOPS per
   `docs/command-loss-timer.md`'s "Secret sourcing" section, not hand-typed
   or reused from the bench.

## Disarmed-mode behavior (no secret present)

For completeness, also spot-check the disarmed path once during the same
bench session (does not need dummy loads wired for every relay — a quick
check is enough):

1. Build and flash **without** `secrets.local.ini` present (or with
   `REBOOT2_HMAC_SECRET` commented out):
   ```sh
   pio run -e bench_pioarduino_esp32s3
   ```
2. Confirm `electrical.reboot2.clt.armed` publishes `false` and no other
   `clt.*`/`confirm.*` paths appear at all (they're not created — see
   `docs/command-loss-timer.md` "Armed vs. disarmed").
3. Confirm every relay is still fully controllable manually (PUT / button /
   `electrical.commands.switch.*`) — disarmed mode must never mean "the
   relays stop working," only "the CLT/commit-confirm layer isn't
   protecting them."
4. Re-flash with the bench secret restored before resuming §4 onward.
