
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
  { 1,  "starlinkInverter",  true,  60000 }, // **Relay 1**
  { 2,  "cellModem",         false, 5000 }, // **Relay 2**
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




void setup() {
  SetupLogging(ESP_LOG_DEBUG);
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
