
#include <memory>

#include "sensesp.h"
#include "sensesp/controllers/smart_switch_controller.h"
#include "sensesp/sensors/analog_input.h"
#include "sensesp/sensors/digital_input.h"
#include "sensesp/sensors/digital_output.h"
#include "sensesp/sensors/sensor.h"
#include "sensesp/signalk/signalk_output.h"
#include "sensesp/signalk/signalk_put_request_listener.h"
#include "sensesp/system/lambda_consumer.h"
#include "sensesp/transforms/repeat.h"
#include "sensesp_app_builder.h"
#include "sensesp/signalk/signalk_value_listener.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <Preferences.h>
#include "esp_system.h"

using namespace sensesp;



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
  { 1,  "starlinkInverter",  false,  60000 }, // **Relay 1**
  { 2,  "cellModem",         false, 60000 }, // **Relay 2**
  { 41, "pepRouter",         false, 60000 }, // **Relay 3**
  { 42, "dataHub",           false, 60000 }, // **Relay 4**
  { 45, "fleetOne",          false, 60000 }, // **Relay 5**
  { 46, "relay6",            true,  60000 }  // **Relay 6**
};

String getSkPath(const String& relayName) {
  return "electrical." + groupName + "." + relayName + ".state";
}

String getSkOutput(const String& relayName) {
  return "/sensesp-" + relayName;
}

////////////////////////////////////////////////////////
// Function to perform a reboot sequence on a normally open or normally closed relay
// along with a SignalK plugin to monitor devices it can set a reboot
// command that will trigger the reboot sequence
// Ive also set up a Put listener so any device on the network can trigger a reboot
// by sending a PUT request with the value "reboot" to the state path of the relay
////////////////////////////////////////////////////////

void reboot_sequence(SmartSwitchController* controller,
          uint32_t on_ms, bool contact_type) {
  if (!contact_type) {
    controller->emit(true);
    event_loop()->onDelay(on_ms, [controller] { controller->emit(false); });
  } else {
    controller->emit(false);
    event_loop()->onDelay(on_ms, [controller] { controller->emit(true); });
  }
}

SmartSwitchController* initialize_relay(uint8_t pin, String sk_path,
                                        String config_path_sk_output,
                                        bool contact_type = false,
                                        int reboot_time_ms = 60000
                              ) {
  // Initialize the relay pin to output
  pinMode(pin, OUTPUT);
  // Set the relay GPIO pins to LOW (off) initially
  digitalWrite(pin, LOW);
  auto* load_switch = new DigitalOutput(pin);

  // Create a switch controller to handle the user press logic and
  // connect it to the load switch...
  SmartSwitchController* controller = new SmartSwitchController(true);
  controller->connect_to(load_switch);

  // In addition to the manual button "click types", a
  // SmartSwitchController accepts explicit state settings via
  // any boolean producer as well as any "truth" values in human readable
  // format via a String producer.
  // Here, we set up a SignalK PUT request listener to handle
  // requests made to the Signal K server to set the switch state.
  // This allows any device on the SignalK network that can make
  // such a request to also control the state of our switch.

  auto* sk_listener = new StringSKPutRequestListener(sk_path);

  sk_listener->connect_to(controller->truthy_string_consumer_);

  sk_listener->connect_to(new LambdaConsumer<String>(
      [controller, contact_type, reboot_time_ms](String value) {
    if (value == "reboot") {
        reboot_sequence(controller, reboot_time_ms, contact_type);
    }
    }));


  load_switch->connect_to(new Repeat<bool, bool>(600000))
      ->connect_to(new SKOutputBool(sk_path, config_path_sk_output));

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
      [controller, contact_type, reboot_time_ms](bool value) {
    if (value) {
        reboot_sequence(controller, reboot_time_ms, contact_type);
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
//   Post-reboot: 10-min hold-off before resuming monitoring.
//   Circuit breaker: max 3 router reboots. Reset after 7 days of clean pings.
//
// NVS namespace "watchdog" is cleared on ESP_RST_POWERON and preserved across
// ESP.restart() so the 23h/24h counters survive the Stage 1 soft restart.

static const IPAddress WATCHDOG_TARGET(192, 168, 22, 1);
static const uint16_t  WATCHDOG_PORT            = 80;
static const uint32_t  FAIL_ESP_RESTART_SECS    = 23 * 3600;    // restart ESP at 23h
static const uint32_t  FAIL_ROUTER_REBOOT_SECS  = 24 * 3600;    // reboot router at 24h
static const uint32_t  HOLDOFF_SECS             = 600;           // 10-min post-reboot hold-off
static const uint32_t  CLEAN_RESET_SECS         = 7 * 24 * 3600; // 7 days clean → reset counter
static const uint8_t   MAX_ROUTER_REBOOTS       = 3;

static bool router_alive() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  bool ok = client.connect(WATCHDOG_TARGET, WATCHDOG_PORT, 1500);
  client.stop();
  return ok;
}

static void watchdog_tick(SmartSwitchController* router_ctrl) {
  Preferences p;
  p.begin("watchdog", false);

  // Post-reboot hold-off: give NarwhalCore time to fully boot before resuming
  if (p.getBool("holdoff", false)) {
    uint32_t h = p.getUInt("holdoff_s", 0) + 60;
    if (h >= HOLDOFF_SECS) {
      p.putBool("holdoff", false);
      p.putUInt("holdoff_s", 0);
      p.putUInt("fail_s", 0);
      ESP_LOGI("WD", "Hold-off complete — monitoring resumed");
    } else {
      p.putUInt("holdoff_s", h);
      ESP_LOGI("WD", "Post-reboot hold-off: %u/%u s", h, HOLDOFF_SECS);
    }
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
    p.end();
    reboot_sequence(router_ctrl, 60000, false);  // NC: cut power 60s then restore
    return;
  }

  p.end();
}


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
                    ->enable_ota("transport")
                    ->get_app();

  // initialize the relays and write up everything to Signal K

  auto relay_controller1 = initialize_relay(relays[0].pin,
                        getSkPath(relays[0].name),
                        getSkOutput(relays[0].name),
                        relays[0].NO, relays[0].ms);
  auto relay_controller2 = initialize_relay(relays[1].pin,
                        getSkPath(relays[1].name),
                        getSkOutput(relays[1].name),
                        relays[1].NO, relays[1].ms);
  auto relay_controller3 = initialize_relay(relays[2].pin,
                        getSkPath(relays[2].name),
                        getSkOutput(relays[2].name),
                        relays[2].NO, relays[2].ms);
  auto relay_controller4 = initialize_relay(relays[3].pin,
                        getSkPath(relays[3].name),
                        getSkOutput(relays[3].name),
                        relays[3].NO, relays[3].ms);
  auto relay_controller5 = initialize_relay(relays[4].pin,
                        getSkPath(relays[4].name),
                        getSkOutput(relays[4].name),
                        relays[4].NO, relays[4].ms);
  auto relay_controller6 = initialize_relay(relays[5].pin,
                        getSkPath(relays[5].name),
                        getSkOutput(relays[5].name),
                        relays[5].NO, relays[5].ms);

  // Register router watchdog — checks every 60s, acts on pepRouter relay (relay 3)
  event_loop()->onRepeat(60000, [relay_controller3]() {
    watchdog_tick(relay_controller3);
  });
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
