# SensESP Remote Switching Controller

> **⚠️ BREAKING CHANGE — v2-powered semantics (fixer #1224, ADR 0055 §4)**
>
> As of this version, the SignalK wire value for **every** relay's `.state`
> path (and the `electrical.commands.switch.*` / `electrical.commands.reboot.*`
> command paths) means **"is the device POWERED"** — `true` = powered,
> `false` = unpowered — for both NC and NO relays. Previously the wire value
> was the raw coil energize state, which meant `true` actually meant
> "powered OFF" for the four NC relays (starlinkInverter, cellModem,
> pepRouter, dataHub). Any app-side code that used to invert NC values to
> compensate must have that inversion **removed** (cruising-app `f4377e7`).
> This firmware publishes `electrical.reboot2.semantics = "v2-powered"`
> (on connect and every 60s) so app code can detect the new firmware and
> gate actuation on it — check for that exact string before assuming
> uninverted semantics, to avoid double-inverting during a mixed-version
> window.
>
> **The firmware and app-side changes deploy in the SAME attended window.**
> Flashing this firmware is attended-only (see repo guardrails below) —
> do not flash without also deploying the corresponding cruising-app change.
>
> The physical boot/reset/brownout behavior is UNCHANGED: all coils are
> still driven LOW at reset before any SensESP code runs, which still means
> all 4 NC loads are powered and both NO loads are unpowered, independent of
> firmware/WiFi/SignalK state. Only the SignalK-facing meaning of the wire
> value changed.
>
> | Relay | Name | Contact | Wire value `true` → physical effect | Boot state (coil LOW) |
> |---|---|---|---|---|
> | 1 | starlinkInverter | NC | device powered (coil de-energized) | powered |
> | 2 | cellModem | NC | device powered (coil de-energized) | powered |
> | 3 | pepRouter | NC | device powered (coil de-energized) | powered |
> | 4 | dataHub | NC | device powered (coil de-energized) | powered |
> | 5 | fleetOne | NO | device powered (coil energized) | unpowered |
> | 6 | relay6 (spare) | NO | device powered (coil energized) | unpowered |
>
> A `reboot` command (or `electrical.commands.reboot.*` = true) always means
> "cut power, wait `ms`, restore power" for every relay, regardless of
> contact type.

This repository is specific to the WaveShare 6 relay module.
https://www.waveshare.com/wiki/ESP32-S3-Relay-6CH easily available at Amazon. It can likely be adapted to other devices.

It uses the SensESP framework to connect to a SignalK server.
On startup you should be able to connect to
the WiFi access point with the same name as the device. The password is `thisisfine`. From there you can then configure it to connect with your network and SignalK server.

Comprehensive documentation for SensESP, including how to get started with your own project, is available at the [SensESP documentation site](https://signalk.org/SensESP/).

This allows you to control the six relays using PUT requests from any device on your network. It works extremely well with the SKipper app https://www.skipperapp.net/

It can also be used for network monitoring and perform rebooting of stuck devices. Using a SignalK plugin to monitor network devices and changing of a value on the SignalK server listened to by this device (default "electrical.commands.reboot.+relayName"). Will initiate the reboot sequence of the relay. Because the power on and off are locally controlled on this device things like modems and routers that are critical to connectivity can be safely restarted remotely or automatically. When using to monitor network devices use the NC contact on the WaveShare. This way your device is always on independent of the status of the WaveShare and only turned off if the WaveShare successfully switches.

As well automatic control can be achieved by a SignalK plugin to schedule or react to events by changing a value in SignalK that is listened to by this device. (default "electrical.commands.switch.+relayName")

**For simple installation**, change the section in `main.cpp` that defines the **group name** and **relay names**. Then build and upload to the Waveshare device and connect it to your network and SignalK server.

---

**Example:**

```cpp
// Change the group name here:
const String groupName = "`reboot2`"; // **Group name used in Signal K path**

RelayInfo relays[] = {
  // Pin  Name                  NO    Reboot time (ms)  (true=NO false=NC)
  { 1,  "`starlinkInverter`",  false, 60000 }, // **Relay 1** — Starlink (NC, default ON)
  { 2,  "`cellModem`",         false, 60000 }, // **Relay 2** — HD1 Dome cell (NC, default ON)
  { 41, "`pepRouter`",         false, 60000 }, // **Relay 3** — NarwhalCore (NC, watchdog)
  { 42, "`dataHub`",           false, 60000 }, // **Relay 4**
  { 45, "`fleetOne`",          true,  60000 }, // **Relay 5** — FleetOne sat (NO, default OFF)
  { 46, "`relay6`",            true,  60000 }  // **Relay 6**
};
```

---

**Testing:** The NC/NO wire-semantics mapping (`src/contact_mapping.h`), the
command-loss-timer / commit-confirm layer (`src/command_loss_timer.h`,
`src/commit_confirm.h`, `src/auth_token.h`, `src/hmac_sha256.h`,
`src/wall_clock.h` — see `docs/command-loss-timer.md`), and the internet-probe
consensus / hold-off logic (`src/internet_probe.h` — see
`docs/observability.md`) are pure logic with no Arduino dependency, so they
have plain g++ host tests instead of requiring hardware or the PlatformIO
toolchain:

```sh
for t in contact_mapping hmac_sha256 auth_token command_loss_timer commit_confirm internet_probe; do
  g++ -std=c++17 -Isrc "test/test_${t}.cpp" -o "/tmp/test_${t}" && "/tmp/test_${t}"
done
```

**Command-loss timer / commit-confirm (fixer ADR 0055 §4, job 5/4):** wired
live into `src/main.cpp`'s relay control flow as of this job, gated on
`REBOOT2_HMAC_SECRET` (see `secrets.local.ini.example`) — with no secret at
build time it compiles out entirely and every relay stays fully manual,
loudly published as `electrical.reboot2.clt.armed = false`. See
`docs/command-loss-timer.md` for the armed/disarmed behavior table, the full
`clt.*`/`confirm.*` SignalK path list, and the boot fail-safe verification
note. `pio run -e pioarduino_esp32s3` is green both with and without
`secrets.local.ini` present.

**Watchdog observability:** the router watchdog's fail counters, circuit-
breaker state, and the internet-reachability probe (fixer ADR 0055 §4, job
3/4) are published to SignalK under `electrical.reboot2.watchdog.*` — see
`docs/observability.md` for the full path list and what each means.
