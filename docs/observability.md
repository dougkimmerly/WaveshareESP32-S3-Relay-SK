# Observability + hardening (fixer ADR 0055 §4, job 3/4)

Job 3 of 4 in the ADR 0055 series. Builds on job 1 (v2 "device powered"
NC/NO semantics) and job 2 (command-loss timer / commit-confirm as pure-C++
modules, not yet wired into live control flow). Adds a second watchdog
input for WAN-only outages and makes the router watchdog's internal state
remotely observable (investigation §1.2 — previously not observable at
all).

## Internet probe (`src/internet_probe.h`)

The existing router watchdog (`src/main.cpp` "Router Watchdog" section)
only TCP-probes NarwhalCore on the LAN (`192.168.22.1:80`). It cannot see a
WAN-only outage — the router answering fine locally while its uplink
(Starlink/cell) is down. `src/internet_probe.h` adds that as a second,
independent input:

- **Consensus** (`check_consensus`/`ConsensusResult`): TCP connect to two
  independent, well-known anycast targets (1.1.1.1:443, 8.8.8.8:443 in
  `main.cpp`). Internet is declared down only when **both** fail — one
  target's regional hiccup or rate limit can't false-positive.
- **Multi-condition confirm** (`ConfirmGate`): requires N consecutive
  "down" ticks (5, i.e. 5 minutes at the 60s tick rate) before the outage
  is treated as confirmed; any single "up" tick resets the count.
- **Exponential hold-off** (`ExponentialHoldoff`): `base_secs * 2^n`,
  capped, and its `breaker_open()` never proposes more actions than the
  caller's `max_actions` — in `main.cpp` this replaces the router
  watchdog's previously-fixed 10-minute post-reboot hold-off with one that
  grows per prior reboot (still starting at 10 min for the first reboot),
  capped at 4h, and still gated by the existing `MAX_ROUTER_REBOOTS = 3`
  breaker. This makes repeated flapping strictly *more* patient than
  before — it never fires an action sooner than the old fixed cadence
  would have.

Host tests: `test/test_internet_probe.cpp` covers consensus (both/one/
neither target down), the confirm gate's consecutive-tick requirement and
reset-on-up, and the hold-off's doubling/capping/breaker behavior.

**Scope decision — observability only, no new relay action.** The internet
probe is wired into `main.cpp` and published to SignalK (below), but does
not itself trigger any relay action. Which physical relay(s) to cycle for a
confirmed WAN-only outage — pepRouter (in case NarwhalCore's WAN interface,
not just LAN, is the problem), starlinkInverter, cellModem, or some
combination — is a judgment call about the boat's actual failure modes that
belongs to Doug, not a guess baked into an unattended job. Recorded here
rather than filed as a fixer DB issue (this job's guardrails don't cover
writing to fixer's Postgres) — recommend a follow-up job (job 4) once Doug
has decided the action policy for a confirmed WAN-only outage. The
exponential hold-off applied to the
*existing* router-reboot action is safe to wire live because it can only
make that action fire less often than before, never more.

## SignalK paths

All published every 60s (same tick as the router watchdog / internet
probe), under `electrical.reboot2.watchdog.*`:

| Path | Type | Meaning |
|---|---|---|
| `electrical.reboot2.watchdog.internetReachable` | bool | Raw per-tick consensus result: `false` only when both probe targets failed this tick. |
| `electrical.reboot2.watchdog.internetOutageConfirmed` | bool | `true` once 5 consecutive ticks have all failed consensus (debounced). |
| `electrical.reboot2.watchdog.failSeconds` | int | Seconds NarwhalCore has been unreachable on the LAN (resets to 0 when it answers, or once a post-reboot hold-off completes). |
| `electrical.reboot2.watchdog.breakerOpen` | bool | `true` once the router-reboot circuit breaker has hit `MAX_ROUTER_REBOOTS` (3) — no further reboot action will be taken until 7 days clean resets it. |
| `electrical.reboot2.watchdog.rebootCount` | int | Router reboots triggered since the last power-on reset or 7-day clean reset — the reboot/restart cycle counter. |
| `electrical.reboot2.watchdog.holdoffActive` | bool | `true` while in the post-reboot exponential hold-off window. |

### CLT state / commit-confirm pending state — now published (job 5/4)

The original ask (job 3) also requested publishing command-loss-timer (CLT)
rung / seconds-since-contact and commit-confirm pending state. At the time,
those modules (`src/command_loss_timer.h`, `src/commit_confirm.h`, job 2)
were only `#include`d to prove they compile, not wired into the live relay/
SignalK control flow — publishing SK values for a system that isn't
actually running would have been fabricated, not observable, state. Job
5/4 (fixer ADR 0055 §4 follow-up) wired them in; their real state is now
published under `electrical.reboot2.clt.*` / `electrical.reboot2.confirm.*`
— see `docs/command-loss-timer.md` for the full path list and the armed/
disarmed behavior table.

## OTA password (build-time secret)

`enable_ota()` in `src/main.cpp` previously hardcoded `"transport"` as the
espota auth password. It now reads `REBOOT2_OTA_PASSWORD`, sourced the same
way as `REBOOT2_HMAC_SECRET` (job 2) — see `secrets.local.ini.example` and
`docs/command-loss-timer.md`'s "Secret sourcing" section for the SOPS
pattern. If the flag isn't supplied at build time, `main.cpp` falls back to
the old `"transport"` default and `pio run` emits a `#warning` so that
fallback can't silently ship in a real build.

### Rotation step (bench flash)

1. Rotate the value in this homelab's SOPS store (see the `secrets` skill).
2. Before the next bench/deploy build, decrypt it into `secrets.local.ini`
   (gitignored) the same way as the HMAC secret:
   ```sh
   sops -d path/to/reboot2-ota-password.enc.yaml | \
     awk '{print "-D REBOOT2_OTA_PASSWORD=\x27\"" $0 "\"\x27"}' \
     >> /tmp/flags && printf '[env]\nbuild_flags =\n%s\n' \
     "$(sed 's/^/    /' /tmp/flags)" > secrets.local.ini
   ```
   (adapt to however both secrets are actually stored — the point is the
   decrypted value only ever lands in the gitignored local file.)
3. Flash over OTA using the new password once, or over serial if the old
   password is no longer known device-side and OTA is unavailable.
4. This step is attended-only (bench or aboard) — never run from an
   unattended job, per repo guardrails.
