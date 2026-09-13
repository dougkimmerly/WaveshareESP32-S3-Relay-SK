# Handoff / deferred register

Cross-domain and unattended-job discoveries that need a human decision or a
follow-up job, kept here so they don't evaporate. Newest first.

## 2026-09-13 — same keyword-collision pattern likely on starlinkInverter/fleetOne/pepRouter

Cross-domain FYI from dk-w5 (H35 ruling, 2026-09-12): peplink-narwhal now owns
cell-modem ownership questions; reboot2-fw's bare `cellModem` manifest keyword
made us a false co-candidate. Fixed by narrowing it to `cellModem relay
channel` and adding `modem reboot`/`WAN failover` (`.kb-manifest.yaml`) —
reboot2-fw's true scope is the relay/reboot automation, not the modem
hardware/carrier, and `cellModem` is a real firmware constant
(`src/main.cpp:95`, relay 2) so it couldn't just be dropped.

While fixing this, noticed our manifest also carries bare `starlinkInverter`,
`fleetOne`, and `pepRouter` keywords, and peplink-narwhal's manifest
separately claims `starlink`, `fleetone`, and `peplink` — the same
co-candidacy pattern that triggered this job could recur on "who owns
Starlink/FleetOne/the Peplink" questions. Not acted on (out of this job's
scope, and H35 only ruled on the cell modem specifically) — needs Doug's call
on whether to narrow those too.

(Older entries: the ADR 0055 §4 job series (jobs 1-6) — bench-checklist
deferral, accelerated-time design decision, CLT/commit-confirm unwired-modules
note — was resolved by jobs 5 (6d2c8e1, live wiring) and 6 (c8ad31c,
`BENCH_TIME_SCALE` + `docs/bench-checklist.md`); entries drained 2026-08-12,
full text in git history. The bench soak itself remains attended-only work
tracked as fixer issue #1268 (Doug).)
