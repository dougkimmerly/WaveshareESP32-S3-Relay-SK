// Signal K application template file.
//
// This application demonstrates core SensESP concepts in a very
// concise manner. You can build and upload the application as is
// and observe the value changes on the serial port monitor.
//
// You can use this source file as a basis for your own projects.
// Remove the parts that are not relevant to you, and add your own code
// for external hardware libraries.

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

using namespace sensesp;

// relay GPIO pins
const uint8_t kRelayPin1 = 1;
const uint8_t kRelayPin2 = 2;
const uint8_t kRelayPin3 = 41;
const uint8_t kRelayPin4 = 42;
const uint8_t kRelayPin5 = 45;
const uint8_t kRelayPin6 = 46;
const uint8_t kBuzzerPin = 21;
const uint8_t kRGBLedPin = 38;


// A simple function to perform a reboot sequence on a normally open or normally closed relay 
enum ContactType {NO, NC};
void reboot_sequence(SmartSwitchController* controller, uint32_t on_ms, ContactType contact_type) {
  if (contact_type == NC) {
  controller->emit(true);
  event_loop()->onDelay(on_ms, [controller] { controller->emit(false); });
  }
  else {
  controller->emit(false);
  event_loop()->onDelay(on_ms, [controller] { controller->emit(true); });
  }
}

SmartSwitchController* initialize_relay(uint8_t pin, String sk_path,
                                        String config_path_sk_output, 
                                        ContactType contact_type = NC,
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

  load_switch->connect_to(new Repeat<bool, bool>(10000))
      ->connect_to(new SKOutputBool(sk_path, config_path_sk_output));


  // Setup a ValueListener so changing the value with a SK plugin can cause
  // a reboot sequence for in-net automated network monitoring    
  String reboot_path = "electrical.commands.reboot." 
      + config_path_sk_output.substring(8, config_path_sk_output.length()); 

  auto* reboot_listener = new SKValueListener<bool>(reboot_path);
    reboot_listener->connect_to(new LambdaConsumer<bool>(
      [controller, contact_type, reboot_time_ms](bool value) {
    if (value) {
        reboot_sequence(controller, reboot_time_ms, contact_type);
    }
    }));

  // Setup a PutRequestListener so that any device on the network
  // can cause a reboot sequence for in-net automated network monitoring
  auto* put_reboot_listener = new BoolSKPutRequestListener(reboot_path);
    put_reboot_listener->connect_to(new LambdaConsumer<bool>(
      [controller, contact_type, reboot_time_ms](bool value) {
    if (value) {
        reboot_sequence(controller, reboot_time_ms, contact_type);
    }
    }));
    
    
  return controller;
}




// The setup function performs one-time application initialization.
void setup() {
  SetupLogging(ESP_LOG_DEBUG);

  // Construct the global SensESPApp() object
  SensESPAppBuilder builder;
  sensesp_app = (&builder)
                    // Set a custom hostname for the app.
                    ->set_hostname("reboot1")
                    // Optionally, hard-code the WiFi and Signal K server
                    // settings. This is normally not needed.
                    //->set_wifi_client("My WiFi SSID", "my_wifi_password")
                    //->set_wifi_access_point("My AP SSID", "my_ap_password")
                    //->set_sk_server("192.168.10.3", 80)
                    ->get_app();
                    
  // write up everything to Signal K
  auto relay_controller1 = initialize_relay(kRelayPin1, "electrical.reboot1.navnet.state",
                   "sensesp-navnet", NC, 60000);
  auto relay_controller2 = initialize_relay(kRelayPin2, "electrical.reboot1.powernet.state",
                   "sensesp-powrenet", NC, 60000);
  auto relay_controller3 = initialize_relay(kRelayPin3, "electrical.reboot1.maretronPC.state",
                   "sensesp-maretronPC", NC, 60000);
  auto relay_controller4 = initialize_relay(kRelayPin4, "electrical.reboot1.winPC.state",
                   "sensesp-winPC", NC, 60000);
  auto relay_controller5 = initialize_relay(kRelayPin5, "electrical.reboot1.linPC.state",
                   "sensesp-linPC", NC, 60000);
  auto relay_controller6 = initialize_relay(kRelayPin6, "electrical.reboot1.relay6.state",
                   "sensesp-relay6", NO, 60000);


    while(true)
    {
    loop();
    }

}

void loop() { event_loop()->tick();}
