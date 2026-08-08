
#include <memory>

#include "sensesp.h"
#include "sensesp/controllers/smart_switch_controller.h"
#include "sensesp/sensors/analog_input.h"
#include "sensesp/sensors/constant_sensor.h"
#include "sensesp/sensors/digital_input.h"
#include "sensesp/sensors/digital_output.h"
#include "sensesp/sensors/sensor.h"
#include "sensesp/signalk/signalk_output.h"
#include "sensesp/signalk/signalk_put_request_listener.h"
#include "sensesp/system/lambda_consumer.h"
#include "sensesp/transforms/repeat.h"
#include "sensesp/transforms/transform.h"
#include "sensesp_app_builder.h"
#include "sensesp/signalk/signalk_value_listener.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <Preferences.h>
#include <time.h>
#include "esp_system.h"
#include "esp_timer.h"

#include "contact_mapping.h"

// Command-loss timer + commit-confirm (fixer ADR 0055 §4, job 2/4, wired
// live in job 5/4 follow-up). Pure C++ header-only modules — see
// test/test_command_loss_timer.cpp, test/test_commit_confirm.cpp and
// test/test_auth_token.cpp for their host-native test coverage. Wired into
// the live relay/SignalK control flow below, gated entirely on whether
// REBOOT2_HMAC_SECRET was supplied at build time — see the "CLT / commit-
// confirm runtime" section and docs/command-loss-timer.md.
#include "auth_token.h"
#include "command_loss_timer.h"
#include "commit_confirm.h"
#include "internet_probe.h"

// OTA password (fixer ADR 0055 §4, job 3/4): build-time secret, same
// untracked-local-file + SOPS-sourced pattern as REBOOT2_HMAC_SECRET (see
// secrets.local.ini.example, docs/command-loss-timer.md). Falls back to the
// prior hardcoded value only so a clean checkout with no secrets.local.ini
// still builds — see docs/observability.md "OTA password rotation" for the
// bench-flash rotation step. `pio run` warns loudly when the fallback is in
// use so it can't silently ship in a real build.
#ifndef REBOOT2_OTA_PASSWORD
#define REBOOT2_OTA_PASSWORD "transport"
#warning "REBOOT2_OTA_PASSWORD not set — using insecure default OTA password. See secrets.local.ini.example."
#endif

using namespace sensesp;

// Wraps the pure map_load_coil() logic (contact_mapping.h) as a SensESP
// transform. The same class is used both directions in the signal chain:
// load->coil (driving the physical relay) and coil->load (reporting SK
// state), since the mapping is its own inverse.
class ContactMapTransform : public BooleanTransform {
 public:
  explicit ContactMapTransform(bool is_no) : BooleanTransform(""), is_no_{is_no} {}

  void set(const bool& input) override {
    this->emit(map_load_coil(input, is_no_));
  }

 private:
  bool is_no_;
};



struct RelayInfo {
  uint8_t pin;       // GPIO pin number
  String name;        // Relay name
  bool NO;            // NO or NC
  unsigned long ms;   // Reboot time in milliseconds
};

////////////////////////////////////////////////////////
// Config Section - Edit these to match your application
// Change the groupName and the relayNames
// the paths for SignalK will be generated as a standard format
// you can change the format in the getSkPath, getSkOutput functions,
// and the sk_switch_path and reboot_path variables
// in the initialize_relay function
////////////////////////////////////////////////////////


// Change the group name here:
const String groupName = "reboot2"; // **Group name used in Signal K path**

RelayInfo relays[] = {
  // Pin  Name                  NO    Reboot time (ms)  (true=NO false=NC)
  { 1,  "starlinkInverter",  false,  60000 }, // **Relay 1** — Starlink Maritime (NC, default ON)
  { 2,  "cellModem",         false, 60000 }, // **Relay 2** — HD1 Dome cell modem (rewired 2026-05-25 from old Nighthawk; NC, default ON)
  { 41, "pepRouter",         false, 60000 }, // **Relay 3** — NarwhalCore router (NC, default ON, has 24h watchdog)
  { 42, "dataHub",           false, 60000 }, // **Relay 4**
  { 45, "fleetOne",          true,  60000 }, // **Relay 5** — FleetOne satellite (NO, default OFF — energize to power on; wired 2026-05-25)
  { 46, "relay6",            true,  60000 }  // **Relay 6**
};

String getSkPath(const String& relayName) {
  return "electrical." + groupName + "." + relayName + ".state";
}

String getSkOutput(const String& relayName) {
  return "/sensesp-" + relayName;
}

// ─── CLT / Commit-Confirm runtime (fixer ADR 0055 §4, job 5/4 follow-up) ───
//
// Wires src/command_loss_timer.h + src/commit_confirm.h into the live relay
// control flow. Armed only when REBOOT2_HMAC_SECRET was supplied at build
// time (secrets.local.ini — see secrets.local.ini.example and
// docs/command-loss-timer.md); with no secret this whole block compiles
// out and every relay stays exactly as manual as it was before this job —
// `pio run` stays green either way (acceptance criterion). The armed/
// disarmed state itself is always published to SignalK (see setup()) so
// home can tell the CLT is not protecting the boat without a bench
// connection, even on a build with the secret absent.
//
// relay_id numbering matches the relays[] array position (1 = relays[0] =
// starlinkInverter, ... 6 = relays[5] = relay6), which is also exactly the
// numbering CommitConfirmGuard::is_wan_relay() and the doc's "relays 1-4"
// language already use.
#ifdef REBOOT2_HMAC_SECRET

static reboot2::clt::CommandLossTimer* g_clt = nullptr;
static reboot2::confirm::CommitConfirmGuard* g_confirm = nullptr;

// One controller pointer per logical relay id (index 0 unused) so CLT rung
// actions and commit-confirm reverts can drive relays through the very same
// actuation seam (SmartSwitchController::switch_consumer_ / ::emit) that
// the PUT and electrical.commands.switch.* listeners already use.
static SmartSwitchController* g_relay_ctrl[7] = {nullptr};
static bool g_relay_powered[7] = {false};
// Set true immediately before a system-driven (reboot pulse, CLT rung
// action, commit-confirm revert) call into the actuation seam so the
// observer attached in initialize_relay() below does not re-track its own
// system-issued command — mirrors the "reboot commands exempt" rule these
// other self-justified system actions share. Consumed (reset to false) by
// that same observer the instant it fires.
static bool g_skip_confirm_once[7] = {false};

static void mark_system_driven_relay_change(int relay_id) {
  if (relay_id >= 1 && relay_id <= 6) g_skip_confirm_once[relay_id] = true;
}

// esp_timer_get_time() is a 64-bit microsecond counter that never wraps in
// any realistic uptime (unlike millis(), which wraps every ~49.7 days —
// unacceptable for a timer meant to track weeks of no-contact during
// stored-boat). This is the CLT's now_mono.
static uint64_t clt_now_mono() {
  return static_cast<uint64_t>(esp_timer_get_time() / 1000000LL);
}

// ESP32 Arduino's SNTP-backed time() is itself an anchor + monotonic-drift
// clock, the same model wall_clock.h implements — reusing it here is the
// smallest correct way to give the CLT/commit-confirm freshness check
// (auth::verify_and_accept) a real wall-clock reading without hand-rolling
// an NTP client (configTime() below arms the SDK's built-in SNTP client).
// Returns 0 (sentinel: "no wall-clock yet") before the first sync lands,
// which fails every token's freshness check safely closed rather than
// open — no worse than the CLT not being wired at all.
static uint64_t clt_now_wall() {
  time_t t = time(nullptr);
  return t > 1700000000 ? static_cast<uint64_t>(t) : 0;
}

#else  // !REBOOT2_HMAC_SECRET

static void mark_system_driven_relay_change(int /*relay_id*/) {}

#endif  // REBOOT2_HMAC_SECRET

////////////////////////////////////////////////////////
// Function to perform a reboot sequence on a normally open or normally closed relay
// along with a SignalK plugin to monitor devices it can set a reboot
// command that will trigger the reboot sequence
// Ive also set up a Put listener so any device on the network can trigger a reboot
// by sending a PUT request with the value "reboot" to the state path of the relay
////////////////////////////////////////////////////////

// Marks relay_id's next state change as system-driven (reboot pulse, or a
// CLT rung action) so the commit-confirm observer (see "CLT / commit-
// confirm runtime" below) does not track it as an externally-sourced
// command needing a confirm — same exemption the commit-confirm module
// already gives reboots by construction (CommitConfirmGuard::submit's
// is_reboot param). A no-op when the CLT isn't armed. Forward-declared here
// so reboot_sequence (used by both the plain reboot listeners and, later,
// the CLT's power-cycle rung) can mark itself exempt regardless of caller.
static void mark_system_driven_relay_change(int relay_id);

// Uniform load semantics (v2-powered): the controller always operates in
// terms of "is the device powered", regardless of NC/NO contact type — the
// ContactMapTransform on its output handles the physical inversion. A
// reboot is always: cut power, wait, restore power. relay_id (1-6, matching
// the relays[] array position) is used only to exempt this cut from
// commit-confirm tracking, same as any other system-driven action — pass 0
// for a relay that has no logical id (there is none in this firmware, but
// keeps the parameter meaningful rather than assumed).
void reboot_sequence(SmartSwitchController* controller, int relay_id, uint32_t on_ms) {
  mark_system_driven_relay_change(relay_id);
  controller->emit(false);
  event_loop()->onDelay(on_ms, [controller] { controller->emit(true); });
}

SmartSwitchController* initialize_relay(uint8_t pin, String sk_path,
                                        String config_path_sk_output,
                                        int relay_id,
                                        bool contact_type = false,
                                        int reboot_time_ms = 60000
                              ) {
  // Initialize the relay pin to output
  pinMode(pin, OUTPUT);
  // Set the relay GPIO pins to LOW (off) initially. This is the SACRED boot
  // fail-safe: at reset/brownout every coil is LOW, which de-energizes all
  // 6 relays. For the 4 NC relays that means the load is POWERED with no
  // firmware involvement; for the 2 NO relays it means unpowered. Nothing
  // downstream of this line may change that physical reset behavior.
  digitalWrite(pin, LOW);
  auto* load_switch = new DigitalOutput(pin);

  // Create a switch controller to handle the user press logic. Its output
  // is uniform "load powered" semantics (v2-powered) for every relay,
  // regardless of contact type — auto-initialize is disabled here because
  // the correct initial value depends on contact_type (below), not on the
  // library's built-in "off" default.
  SmartSwitchController* controller = new SmartSwitchController(false);

  // The controller drives the physical coil through a ContactMapTransform,
  // which is the only place NC/NO inversion happens.
  controller->connect_to(new ContactMapTransform(contact_type))
      ->connect_to(load_switch);

#ifdef REBOOT2_HMAC_SECRET
  // Commit-confirm interposition (fixer ADR 0055 §4, job 5/4 follow-up):
  // this parallel observer sees every state change the controller emits,
  // from whichever source drove it — the PUT listener, the
  // electrical.commands.switch.* listener, the physical button, a reboot
  // pulse, or a CLT rung action — because they all funnel through this one
  // controller's emit()/switch_consumer_/truthy_string_consumer_. That
  // makes this the single actuation seam the job asked for: reboot pulses
  // and CLT rung actions mark themselves exempt via
  // mark_system_driven_relay_change() before they act (consumed here as
  // `skip`), so only externally-sourced WAN-chain (relays 1-4) power-cuts
  // ever get tracked as provisional by CommitConfirmGuard::submit().
  g_relay_ctrl[relay_id] = controller;
  g_relay_powered[relay_id] = map_load_coil(false, contact_type);  // matches the boot-derived initial emit below
  controller->attach([relay_id, controller]() {
    bool new_state = controller->get();
    bool prior = g_relay_powered[relay_id];
    if (new_state == prior) return;
    bool skip = g_skip_confirm_once[relay_id];
    g_skip_confirm_once[relay_id] = false;
    if (g_confirm != nullptr) {
      g_confirm->submit(relay_id, new_state, prior, /*is_reboot=*/skip, clt_now_mono());
    }
    g_relay_powered[relay_id] = new_state;
  });
#endif

  // In addition to the manual button "click types", a
  // SmartSwitchController accepts explicit state settings via
  // any boolean producer as well as any "truth" values in human readable
  // format via a String producer.
  // Here, we set up a SignalK PUT request listener to handle
  // requests made to the Signal K server to set the switch state.
  // This allows any device on the SignalK network that can make
  // such a request to also control the state of our switch.
  // The wire value here always means "device POWERED" (v2-powered), for
  // both NC and NO relays.

  auto* sk_listener = new StringSKPutRequestListener(sk_path);

  sk_listener->connect_to(controller->truthy_string_consumer_);

  sk_listener->connect_to(new LambdaConsumer<String>(
      [controller, relay_id, reboot_time_ms](String value) {
    if (value == "reboot") {
        reboot_sequence(controller, relay_id, reboot_time_ms);
    }
    }));

  // Report LOAD state (device powered), not raw coil state — mirror the
  // coil back through the same (self-inverse) ContactMapTransform.
  load_switch->connect_to(new ContactMapTransform(contact_type))
      ->connect_to(new Repeat<bool, bool>(600000))
      ->connect_to(new SKOutputBool(sk_path, config_path_sk_output));

  // Emit the correct initial load state once the event loop starts, derived
  // from the known boot coil state (LOW/false) rather than the library's
  // "off" default — this is what keeps NC relays reporting "powered" at
  // boot instead of the controller incorrectly re-energizing the coil.
  event_loop()->onDelay(0, [controller, contact_type]() {
    controller->emit(map_load_coil(false, contact_type));
  });

  // Setup a ValueListener so changing the value with a SK plugin can cause
  // the relay to turn on or off
  String sk_switch_path = "electrical.commands.switch."
      + config_path_sk_output.substring(9, config_path_sk_output.length());

  auto* sk_listener2 = new SKValueListener<String>(sk_switch_path);
    sk_listener2->connect_to(controller->truthy_string_consumer_);

  // Setup a ValueListener so changing the value with a SK plugin can cause
  // a reboot sequence for in-net automated network monitoring
  String reboot_path = "electrical.commands.reboot."
      + config_path_sk_output.substring(9, config_path_sk_output.length());

  auto* reboot_listener = new SKValueListener<bool>(reboot_path);
    reboot_listener->connect_to(new LambdaConsumer<bool>(
      [controller, relay_id, reboot_time_ms](bool value) {
    if (value) {
        reboot_sequence(controller, relay_id, reboot_time_ms);
    }
    }));




  return controller;
}


// ─── Router Watchdog ──────────────────────────────────────────────────────────
//
// Monitors NarwhalCore (192.168.22.1:80). If unreachable for 23h, restarts the
// ESP32 to rule out a corrupted WiFi stack. If still unreachable at 24h,
// power-cycles the router via the pepRouter relay (NC, relay 3, pin 41).
//
// Detection ladder:
//   Every 60s: TCP connect to 192.168.22.1:80 (1.5s timeout).
//   Failure is accumulated in NVS flash — survives ESP.restart().
//   At 23h: ESP.restart() (rules out ESP WiFi stack as the problem).
//   At 24h: trigger pepRouter relay reboot sequence (60s power cut).
//   Post-reboot: hold-off before resuming monitoring, growing exponentially
//   per prior action (src/internet_probe.h ExponentialHoldoff) — strictly
//   more patient than the old fixed 10-min hold-off, never less.
//   Circuit breaker: max 3 router reboots. Reset after 7 days of clean pings.
//
// NVS namespace "watchdog" is cleared on ESP_RST_POWERON and preserved across
// ESP.restart() so the 23h/24h counters survive the Stage 1 soft restart.
//
// This LAN-only probe cannot see a WAN-only outage (router answers fine on
// the LAN, but its internet uplink is down) — see the Internet Probe section
// below (fixer ADR 0055 §4, job 3/4, investigation §1.2) for that input.
// It is observability-only for now: which relay(s) to act on for a
// WAN-only outage is a judgment call (Starlink vs. cell modem vs. router)
// that belongs to Doug, not a guess baked into an unattended job — see
// docs/observability.md.

static const IPAddress WATCHDOG_TARGET(192, 168, 22, 1);
static const uint16_t  WATCHDOG_PORT            = 80;
static const uint32_t  FAIL_ESP_RESTART_SECS    = 23 * 3600;    // restart ESP at 23h
static const uint32_t  FAIL_ROUTER_REBOOT_SECS  = 24 * 3600;    // reboot router at 24h
static const uint32_t  HOLDOFF_BASE_SECS        = 600;           // 1st post-reboot hold-off
static const uint32_t  HOLDOFF_MAX_SECS         = 4 * 3600;      // hold-off cap
static const uint32_t  CLEAN_RESET_SECS         = 7 * 24 * 3600; // 7 days clean → reset counter
static const uint8_t   MAX_ROUTER_REBOOTS       = 3;

static reboot2::netprobe::ExponentialHoldoff g_router_holdoff(HOLDOFF_BASE_SECS, HOLDOFF_MAX_SECS,
                                                               MAX_ROUTER_REBOOTS);

static bool router_alive() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  bool ok = client.connect(WATCHDOG_TARGET, WATCHDOG_PORT, 1500);
  client.stop();
  return ok;
}

// ─── Internet Probe ────────────────────────────────────────────────────────
//
// Second watchdog input (fixer ADR 0055 §4, job 3/4, investigation §1.2):
// TCP connect to two independent, well-known anycast targets on 443 every
// 60s. Internet is declared "down" only on consensus of both (src/
// internet_probe.h ConsensusResult::internet_down()) so one target's
// regional/rate-limit hiccup can't false-positive. INTERNET_CONFIRM_TICKS
// consecutive down ticks (ConfirmGate) are required before the
// confirmed-outage state is published — debounces a single bad tick. This
// is observability-only: it publishes state to SignalK, it does not
// trigger any relay action (see note above).

static const IPAddress INTERNET_PROBE_A(1, 1, 1, 1);   // Cloudflare
static const IPAddress INTERNET_PROBE_B(8, 8, 8, 8);   // Google
static const uint16_t  INTERNET_PROBE_PORT       = 443;
static const uint8_t   INTERNET_CONFIRM_TICKS    = 5;   // 5 consecutive 60s ticks = 5 min

static reboot2::netprobe::ConfirmGate g_internet_confirm(INTERNET_CONFIRM_TICKS);

static bool internet_probe(const IPAddress& target) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  bool ok = client.connect(target, INTERNET_PROBE_PORT, 1500);
  client.stop();
  return ok;
}

// ─── Watchdog Observability (SignalK) ──────────────────────────────────────
//
// Publishes the watchdog's internal state — previously not remotely
// observable at all (investigation §1.2) — so the home dead-man and
// post-incident forensics can see it without a bench connection. Created
// once in setup(); the tick functions below call ->set() on these each
// pass. See README.md for the full path list.

static SKOutputBool* g_sk_internet_reachable        = nullptr;
static SKOutputBool* g_sk_internet_outage_confirmed = nullptr;
static SKOutputInt*  g_sk_watchdog_fail_seconds     = nullptr;
static SKOutputBool* g_sk_watchdog_breaker_open     = nullptr;
static SKOutputInt*  g_sk_watchdog_reboot_count     = nullptr;
static SKOutputBool* g_sk_watchdog_holdoff_active   = nullptr;

static void publish_watchdog_state(uint32_t fail_s, uint8_t reboots, bool holdoff_active) {
  if (g_sk_watchdog_fail_seconds) g_sk_watchdog_fail_seconds->set(static_cast<int>(fail_s));
  if (g_sk_watchdog_reboot_count) g_sk_watchdog_reboot_count->set(static_cast<int>(reboots));
  if (g_sk_watchdog_breaker_open) g_sk_watchdog_breaker_open->set(reboots >= MAX_ROUTER_REBOOTS);
  if (g_sk_watchdog_holdoff_active) g_sk_watchdog_holdoff_active->set(holdoff_active);
}

static void watchdog_tick(SmartSwitchController* router_ctrl) {
  Preferences p;
  p.begin("watchdog", false);

  // Post-reboot hold-off: give NarwhalCore time to fully boot before
  // resuming. Target grows exponentially per prior action taken (`reboots`
  // already reflects the action that just triggered this hold-off — see
  // ExponentialHoldoff::holdoff_secs()) so repeated flapping backs off
  // instead of retrying at the same fixed 10-min cadence forever.
  if (p.getBool("holdoff", false)) {
    uint8_t  reboots_so_far = p.getUChar("reboots", 0);
    uint32_t target = g_router_holdoff.holdoff_secs(reboots_so_far > 0 ? reboots_so_far - 1 : 0);
    uint32_t h = p.getUInt("holdoff_s", 0) + 60;
    if (h >= target) {
      p.putBool("holdoff", false);
      p.putUInt("holdoff_s", 0);
      p.putUInt("fail_s", 0);
      ESP_LOGI("WD", "Hold-off complete — monitoring resumed");
    } else {
      p.putUInt("holdoff_s", h);
      ESP_LOGI("WD", "Post-reboot hold-off: %u/%u s", h, target);
    }
    publish_watchdog_state(p.getUInt("fail_s", 0), reboots_so_far, h < target);
    p.end();
    return;
  }

  uint32_t fail_s       = p.getUInt("fail_s", 0);
  uint32_t clean_s      = p.getUInt("clean_s", 0);
  uint8_t  reboots      = p.getUChar("reboots", 0);
  bool     esp_restarted = p.getBool("esp_rst", false);

  if (router_alive()) {
    if (fail_s > 0) {
      ESP_LOGI("WD", "Router back after %.1f h%s",
               fail_s / 3600.0f,
               esp_restarted ? " — WiFi stack was the issue on ESP" : "");
    }
    p.putUInt("fail_s", 0);
    p.putBool("esp_rst", false);
    clean_s += 60;
    p.putUInt("clean_s", clean_s);
    if (clean_s >= CLEAN_RESET_SECS) {
      p.putUChar("reboots", 0);
      p.putUInt("clean_s", 0);
      ESP_LOGI("WD", "7 days clean — router reboot counter reset");
    }
    publish_watchdog_state(0, p.getUChar("reboots", 0), false);
    p.end();
    return;
  }

  // Router unreachable — accumulate failure time
  fail_s  += 60;
  clean_s  = 0;
  p.putUInt("fail_s", fail_s);
  p.putUInt("clean_s", 0);

  ESP_LOGW("WD", "Router unreachable: %.1f h | esp_rst: %s | reboots: %u/%u",
           fail_s / 3600.0f,
           esp_restarted ? "yes" : "no",
           reboots, MAX_ROUTER_REBOOTS);

  // Circuit breaker — stop acting after MAX_ROUTER_REBOOTS
  if (reboots >= MAX_ROUTER_REBOOTS) {
    ESP_LOGE("WD", "Circuit breaker open — %u reboots attempted, manual intervention required",
             reboots);
    publish_watchdog_state(fail_s, reboots, false);
    p.end();
    return;
  }

  // Stage 1 at 23h: restart ESP32 to rule out WiFi stack corruption.
  // NVS is preserved across ESP.restart() so the counter continues from 23h.
  if (fail_s >= FAIL_ESP_RESTART_SECS && !esp_restarted) {
    ESP_LOGW("WD", "23h threshold — restarting ESP32 to rule out WiFi stack issue");
    p.putBool("esp_rst", true);
    p.end();
    delay(200);
    ESP.restart();
    return;  // unreachable, but clear intent
  }

  // Stage 2 at 24h: power-cycle NarwhalCore via pepRouter relay (NC, relay 3)
  if (fail_s >= FAIL_ROUTER_REBOOT_SECS) {
    ESP_LOGE("WD", "24h confirmed — triggering router reboot #%u", reboots + 1);
    p.putUChar("reboots", reboots + 1);
    p.putBool("holdoff", true);
    p.putUInt("holdoff_s", 0);
    p.putBool("esp_rst", false);
    publish_watchdog_state(fail_s, reboots + 1, true);
    p.end();
    reboot_sequence(router_ctrl, /*relay_id=*/3, 60000);  // pepRouter — cut power 60s then restore
    return;
  }

  publish_watchdog_state(fail_s, reboots, false);
  p.end();
}

// Runs every 60s alongside watchdog_tick(). Feeds the two-target consensus
// probe through ConfirmGate before publishing "confirmed outage" so a
// single bad tick doesn't flip that state. Observability-only — see the
// note at the top of the Router Watchdog section for why this does not (yet)
// trigger any relay action.
static void internet_probe_tick() {
  reboot2::netprobe::ConsensusResult result = reboot2::netprobe::check_consensus(
      [] { return internet_probe(INTERNET_PROBE_A); },
      [] { return internet_probe(INTERNET_PROBE_B); });

  bool confirmed_down = g_internet_confirm.tick(result.internet_down());

  if (g_sk_internet_reachable) g_sk_internet_reachable->set(!result.internet_down());
  if (g_sk_internet_outage_confirmed) g_sk_internet_outage_confirmed->set(confirmed_down);

  if (confirmed_down && g_internet_confirm.consecutive_count() == INTERNET_CONFIRM_TICKS) {
    ESP_LOGW("WD", "Internet outage confirmed after %u consecutive failed ticks (WAN-only — "
                    "router LAN may still be up)",
             INTERNET_CONFIRM_TICKS);
  }
}

// ─── CLT / Commit-Confirm observability + tick (fixer ADR 0055 §4, job 5/4
// follow-up) ─────────────────────────────────────────────────────────────
//
// electrical.reboot2.clt.armed is published unconditionally (both armed and
// disarmed builds) so home/forensics can tell from SignalK alone whether
// the CLT is actually protecting the boat, per the job's requirement that
// disarmed mode be "loudly visible on the semantics/status SK paths" — see
// setup(). The rest of this section only exists when armed.

static SKOutputBool* g_sk_clt_armed = nullptr;

#ifdef REBOOT2_HMAC_SECRET

static SKOutputString* g_sk_clt_rung                  = nullptr;
static SKOutputInt*    g_sk_clt_seconds_since_contact = nullptr;
static SKOutputBool*   g_sk_clt_terminal_window_open  = nullptr;
static SKOutputBool*   g_sk_clt_fleetone_window_open  = nullptr;
static SKOutputInt*    g_sk_confirm_pending_count     = nullptr;
static SKOutputBool*   g_sk_confirm_relay_pending[5]  = {nullptr};  // index 1-4 used

static const char* clt_rung_name(reboot2::clt::Rung rung) {
  switch (rung) {
    case reboot2::clt::Rung::kNormal:   return "normal";
    case reboot2::clt::Rung::kRung12h:  return "12h";
    case reboot2::clt::Rung::kRung24h:  return "24h";
    case reboot2::clt::Rung::kTerminal: return "terminal";
  }
  return "unknown";
}

// Applies a CLT-rung- or commit-confirm-revert-driven relay change through
// the single actuation seam (same as initialize_relay wires the PUT/
// commands.switch listeners to) and marks it exempt from commit-confirm
// re-tracking — see mark_system_driven_relay_change().
static void clt_apply_relay(int relay_id, bool powered) {
  SmartSwitchController* ctrl = g_relay_ctrl[relay_id];
  if (ctrl == nullptr) return;
  mark_system_driven_relay_change(relay_id);
  ctrl->switch_consumer_.set(powered);
}

static void publish_clt_confirm_state() {
  if (g_sk_clt_rung) g_sk_clt_rung->set(String(clt_rung_name(g_clt->rung())));
  if (g_sk_clt_terminal_window_open) g_sk_clt_terminal_window_open->set(g_clt->terminal_window_open());
  if (g_sk_clt_fleetone_window_open) g_sk_clt_fleetone_window_open->set(g_clt->fleetone_window_open());
  if (g_sk_clt_seconds_since_contact) {
    uint64_t now_mono = clt_now_mono();
    uint64_t elapsed = now_mono >= g_clt->last_contact_mono() ? now_mono - g_clt->last_contact_mono() : 0;
    g_sk_clt_seconds_since_contact->set(static_cast<int>(elapsed));
  }
  int pending = 0;
  for (int r = 1; r <= 4; r++) {
    bool has_pending = g_confirm->has_pending(r);
    if (has_pending) pending++;
    if (g_sk_confirm_relay_pending[r]) g_sk_confirm_relay_pending[r]->set(has_pending);
  }
  if (g_sk_confirm_pending_count) g_sk_confirm_pending_count->set(pending);
}

// Runs every 60s: advances the CLT and commit-confirm state machines and
// applies whatever Actions/Reverts they return through the actuation seam,
// then publishes the resulting state to SignalK. This is the only place
// CLT Actions turn into real relay commands.
static void clt_confirm_tick() {
  uint64_t now_mono = clt_now_mono();
  uint64_t now_wall = clt_now_wall();
  if (now_wall > 0) g_clt->sync_wall_clock(now_wall, now_mono);

  for (reboot2::clt::Action action : g_clt->tick(now_mono)) {
    switch (action) {
      case reboot2::clt::Action::kForceWanRelaysOn:
        ESP_LOGW("CLT", "T+12h no authenticated home contact — forcing WAN relays 1-4 ON");
        for (int r = 1; r <= 4; r++) clt_apply_relay(r, true);
        break;
      case reboot2::clt::Action::kPowerCycleStarlinkAndPep:
        ESP_LOGW("CLT", "T+24h no authenticated home contact — power-cycling starlinkInverter + pepRouter");
        reboot_sequence(g_relay_ctrl[1], 1, 60000);
        reboot_sequence(g_relay_ctrl[3], 3, 60000);
        break;
      case reboot2::clt::Action::kEnterTerminalPosture:
        ESP_LOGE("CLT", "T+48h no authenticated home contact — entering terminal posture");
        break;
      case reboot2::clt::Action::kExitToNormal:
        ESP_LOGI("CLT", "authenticated home contact restored — CLT back to Normal");
        break;
      case reboot2::clt::Action::kTerminalWindowOpen:
        ESP_LOGI("CLT", "terminal posture: daily WAN-chain window open (16:00 UTC)");
        for (int r = 1; r <= 4; r++) clt_apply_relay(r, true);
        break;
      case reboot2::clt::Action::kTerminalWindowClose:
        ESP_LOGI("CLT", "terminal posture: daily WAN-chain window closed (18:00 UTC)");
        for (int r = 1; r <= 4; r++) clt_apply_relay(r, false);
        break;
      case reboot2::clt::Action::kFleetOneWindowOpen:
        ESP_LOGI("CLT", "FleetOne daily window open (16:00 UTC)");
        clt_apply_relay(5, true);
        break;
      case reboot2::clt::Action::kFleetOneWindowClose:
        ESP_LOGI("CLT", "FleetOne daily window closed (16:30 UTC)");
        clt_apply_relay(5, false);
        break;
      case reboot2::clt::Action::kNone:
        break;
    }
  }

  for (const reboot2::confirm::Revert& revert : g_confirm->tick(now_mono)) {
    ESP_LOGW("CONFIRM", "relay%d power-cut unconfirmed after 15 min — reverting to powered=%s",
             revert.relay_id, revert.revert_to_powered_state ? "true" : "false");
    clt_apply_relay(revert.relay_id, revert.revert_to_powered_state);
  }

  publish_clt_confirm_state();
}

#endif  // REBOOT2_HMAC_SECRET


void setup() {
  SetupLogging(ESP_LOG_DEBUG);

  // Clear watchdog NVS on a true power-on reset (not ESP.restart()).
  // This gives everything time to boot fresh without accumulating stale failures.
  if (esp_reset_reason() == ESP_RST_POWERON) {
    Preferences p;
    p.begin("watchdog", false);
    p.clear();
    p.end();
    ESP_LOGI("WD", "Power-on reset — watchdog state cleared");
  }

  // Construct the global SensESPApp() object
  SensESPAppBuilder builder;
  sensesp_app = (&builder)
                    // Set a custom hostname for the app.
                    ->set_hostname(groupName)
                    // Optionally, hard-code the WiFi and Signal K server
                    // settings. This is normally not needed.
                    // ->set_wifi_client("Manta", "Blacksmith49")
                    //->set_wifi_access_point("My AP SSID", "my_ap_password")
                    // ->set_sk_server("192.168.22.14", 80)
                    ->enable_ota(REBOOT2_OTA_PASSWORD)
                    ->get_app();

  // Watchdog + internet-probe observability outputs (fixer ADR 0055 §4,
  // job 3/4) — created once here, ->set() from watchdog_tick() /
  // internet_probe_tick() below. See README.md for the path list.
  g_sk_internet_reachable = new SKOutputBool(
      "electrical." + groupName + ".watchdog.internetReachable", "/sensesp-watchdogInternetReachable");
  g_sk_internet_outage_confirmed = new SKOutputBool(
      "electrical." + groupName + ".watchdog.internetOutageConfirmed",
      "/sensesp-watchdogInternetOutageConfirmed");
  g_sk_watchdog_fail_seconds = new SKOutputInt(
      "electrical." + groupName + ".watchdog.failSeconds", "/sensesp-watchdogFailSeconds");
  g_sk_watchdog_breaker_open = new SKOutputBool(
      "electrical." + groupName + ".watchdog.breakerOpen", "/sensesp-watchdogBreakerOpen");
  g_sk_watchdog_reboot_count = new SKOutputInt(
      "electrical." + groupName + ".watchdog.rebootCount", "/sensesp-watchdogRebootCount");
  g_sk_watchdog_holdoff_active = new SKOutputBool(
      "electrical." + groupName + ".watchdog.holdoffActive", "/sensesp-watchdogHoldoffActive");

  // initialize the relays and write up everything to Signal K

  // relay_id (2nd-to-last arg, 1-6) matches the relays[] array position —
  // also the numbering CommitConfirmGuard::is_wan_relay() and the CLT
  // wiring below use for relays 1-4 (the WAN chain).
  auto relay_controller1 = initialize_relay(relays[0].pin,
                        getSkPath(relays[0].name),
                        getSkOutput(relays[0].name),
                        1, relays[0].NO, relays[0].ms);
  auto relay_controller2 = initialize_relay(relays[1].pin,
                        getSkPath(relays[1].name),
                        getSkOutput(relays[1].name),
                        2, relays[1].NO, relays[1].ms);
  auto relay_controller3 = initialize_relay(relays[2].pin,
                        getSkPath(relays[2].name),
                        getSkOutput(relays[2].name),
                        3, relays[2].NO, relays[2].ms);
  auto relay_controller4 = initialize_relay(relays[3].pin,
                        getSkPath(relays[3].name),
                        getSkOutput(relays[3].name),
                        4, relays[3].NO, relays[3].ms);
  auto relay_controller5 = initialize_relay(relays[4].pin,
                        getSkPath(relays[4].name),
                        getSkOutput(relays[4].name),
                        5, relays[4].NO, relays[4].ms);
  auto relay_controller6 = initialize_relay(relays[5].pin,
                        getSkPath(relays[5].name),
                        getSkOutput(relays[5].name),
                        6, relays[5].NO, relays[5].ms);

  // Register router watchdog — checks every 60s, acts on pepRouter relay (relay 3)
  event_loop()->onRepeat(60000, [relay_controller3]() {
    watchdog_tick(relay_controller3);
  });

  // Register internet-reachability probe — checks every 60s, observability
  // only (fixer ADR 0055 §4, job 3/4; see the Internet Probe section above).
  event_loop()->onRepeat(60000, []() { internet_probe_tick(); });

  // Semantics marker (fixer #1224 / ADR 0055 §4): app-side code (cruising-app,
  // the powerNet PNP reconciler) gates actuation on this exact string to
  // avoid double-inverting NC relay commands. Emitted once at startup and
  // every 60s thereafter so it's picked up on (re)connect.
  auto* semantics_marker = new StringConstantSensor(String("v2-powered"), 60);
  semantics_marker->connect_to(
      new SKOutputString("electrical." + groupName + ".semantics"));

  // CLT / commit-confirm runtime (fixer ADR 0055 §4, job 5/4 follow-up).
  // g_sk_clt_armed is published either way — see the "CLT / Commit-Confirm
  // observability" section above for why disarmed-mode visibility matters.
  g_sk_clt_armed = new SKOutputBool(
      "electrical." + groupName + ".clt.armed", "/sensesp-cltArmed");
#ifdef REBOOT2_HMAC_SECRET
  g_clt = new reboot2::clt::CommandLossTimer(std::string(REBOOT2_HMAC_SECRET));
  g_confirm = new reboot2::confirm::CommitConfirmGuard(std::string(REBOOT2_HMAC_SECRET));
  g_clt->begin(clt_now_mono());

  // Arms the ESP32 SDK's built-in SNTP client (see clt_now_wall() above) —
  // the CLT/commit-confirm freshness check needs a real wall-clock reading
  // to authenticate tokens at all. Read-only, well-known NTP pool, no
  // relay-safety implication; falls back to clt_now_wall()'s 0 sentinel
  // (fail closed) until the first sync lands.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  g_sk_clt_rung = new SKOutputString("electrical." + groupName + ".clt.rung");
  g_sk_clt_seconds_since_contact = new SKOutputInt(
      "electrical." + groupName + ".clt.secondsSinceContact", "/sensesp-cltSecondsSinceContact");
  g_sk_clt_terminal_window_open = new SKOutputBool(
      "electrical." + groupName + ".clt.terminalWindowOpen", "/sensesp-cltTerminalWindowOpen");
  g_sk_clt_fleetone_window_open = new SKOutputBool(
      "electrical." + groupName + ".clt.fleetoneWindowOpen", "/sensesp-cltFleetoneWindowOpen");
  g_sk_confirm_pending_count = new SKOutputInt(
      "electrical." + groupName + ".confirm.pendingCount", "/sensesp-confirmPendingCount");
  for (int r = 1; r <= 4; r++) {
    g_sk_confirm_relay_pending[r] = new SKOutputBool(
        "electrical." + groupName + ".confirm.relay" + String(r) + ".pending",
        "/sensesp-confirmRelay" + String(r) + "Pending");
  }

  // Authenticated home-contact token: PUT a "<unix_ts>:<64-hex-char HMAC>"
  // string (see src/auth_token.h) to reset the CLT to Normal. Any other
  // traffic (link-layer, unauthenticated PUTs elsewhere) never reaches
  // process_token() and so can never reset the timer — see
  // docs/command-loss-timer.md.
  auto* clt_token_listener =
      new StringSKPutRequestListener("electrical." + groupName + ".clt.contactToken");
  clt_token_listener->connect_to(new LambdaConsumer<String>([](String value) {
    reboot2::auth::Token token;
    if (!reboot2::auth::parse_token(std::string(value.c_str()), &token)) {
      ESP_LOGW("CLT", "contactToken: malformed value, ignoring");
      return;
    }
    if (g_clt->process_token(token, clt_now_wall(), clt_now_mono())) {
      ESP_LOGI("CLT", "authenticated home contact accepted");
    } else {
      ESP_LOGW("CLT", "contactToken: verification failed (stale, replayed, or bad MAC)");
    }
  }));

  // Commit-confirm tokens, one path per WAN-chain relay (1-4): PUT the same
  // "<unix_ts>:<64-hex-char HMAC>" wire format, scoped to that relay via
  // confirm_token_context(), to confirm a pending power-cut before its
  // 15-minute auto-revert deadline.
  for (int r = 1; r <= 4; r++) {
    String path = "electrical." + groupName + ".confirm.relay" + String(r) + ".token";
    auto* confirm_listener = new StringSKPutRequestListener(path);
    confirm_listener->connect_to(new LambdaConsumer<String>([r](String value) {
      reboot2::auth::Token token;
      if (!reboot2::auth::parse_token(std::string(value.c_str()), &token)) {
        ESP_LOGW("CONFIRM", "relay%d token: malformed value, ignoring", r);
        return;
      }
      if (g_confirm->confirm(r, token, clt_now_wall())) {
        ESP_LOGI("CONFIRM", "relay%d power-cut confirmed", r);
      } else {
        ESP_LOGW("CONFIRM", "relay%d confirm failed (stale/replayed/bad MAC, or nothing pending)", r);
      }
    }));
  }

  event_loop()->onRepeat(60000, []() { clt_confirm_tick(); });
  g_sk_clt_armed->set(true);
  ESP_LOGI("CLT", "armed — REBOOT2_HMAC_SECRET present");
#else
  g_sk_clt_armed->set(false);
  ESP_LOGW("CLT", "DISARMED — no REBOOT2_HMAC_SECRET at build time; all relays fully manual, "
                   "no command-loss protection is running. See secrets.local.ini.example.");
#endif
  // Republish armed/disarmed every 60s too (same reconnect rationale as the
  // semantics marker above) so it's picked up on SK server (re)connect, not
  // only at boot.
  event_loop()->onRepeat(60000, []() { g_sk_clt_armed->set(g_sk_clt_armed->get()); });
}

void loop() {
  static unsigned long last_debug_print = 0;
  unsigned long now = millis();

  // Print WiFi debug info every 10 seconds, but only for the first 5 minutes after restart
  if (now < 300000 && now - last_debug_print > 10000) {
    last_debug_print = now;

    ESP_LOGI("WIFI_DEBUG", "========== Network Status ==========");
    ESP_LOGI("WIFI_DEBUG", "WiFi Status: %d", WiFi.status());
    ESP_LOGI("WIFI_DEBUG", "WiFi Connected: %s", WiFi.status() == WL_CONNECTED ? "YES" : "NO");
    ESP_LOGI("WIFI_DEBUG", "SSID: %s", WiFi.SSID().c_str());
    ESP_LOGI("WIFI_DEBUG", "IP Address: %s", WiFi.localIP().toString().c_str());
    ESP_LOGI("WIFI_DEBUG", "Gateway: %s", WiFi.gatewayIP().toString().c_str());
    ESP_LOGI("WIFI_DEBUG", "Subnet: %s", WiFi.subnetMask().toString().c_str());
    ESP_LOGI("WIFI_DEBUG", "DNS: %s", WiFi.dnsIP().toString().c_str());
    ESP_LOGI("WIFI_DEBUG", "MAC Address: %s", WiFi.macAddress().c_str());
    ESP_LOGI("WIFI_DEBUG", "RSSI: %d dBm", WiFi.RSSI());
    ESP_LOGI("WIFI_DEBUG", "Hostname: %s", WiFi.getHostname());
    ESP_LOGI("WIFI_DEBUG", "====================================");
  }

  event_loop()->tick();
}
