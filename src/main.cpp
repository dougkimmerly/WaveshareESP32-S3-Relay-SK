
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



struct RelayInfo {
  uint8_t pin;       // GPIO pin number
  String name;        // Relay name
  bool NO;            // NO or NC
  unsigned long ms;   // Reboot time in milliseconds
};

////////////////////////////////////////////////////////
// Config Section - Edit these to match your application
// Name the device and the relays
// the paths for SignalK will be generated as a standard format
// you can change the format in the getSkPath, getSkOutput functions
// and the reboot_path variable in the initialize_relay function
////////////////////////////////////////////////////////


const String groupName = "reboot2";  // Name of this group of relays - used in the Signal K path
RelayInfo relays[] = {
// Pin  Name            NO/NC Reboot time in milliseconds  
  { 1,"starlinkInverter", true, 60000 },   // true = NO, false = NC  
  { 2,"cellModem", false, 60000 },  
  { 41,"pepRouter", false, 60000 },  
  { 42,"dataHub", false, 60000 },  
  { 45,"fleetOne", false, 60000 },  
  { 46,"relay6", true, 60000 }  
};

String getSkPath(const String& relayName) {
  return "electrical." + groupName + "." + relayName + ".state";
}

String getSkOutput(const String& relayName) {
  return "sensesp-" + relayName;
}

////////////////////////////////////////////////////////
// Function to perform a reboot sequence on a normally open or normally closed relay 
// along with a SignalK plugin to monitor devices it can set a reboot
// command that will trigger the reboot sequence
// Ive also set up a Put listener so any device on the network can trigger a reboot
// but haven't got it working yet.
void reboot_sequence(SmartSwitchController* controller, uint32_t on_ms, bool contact_type) {
  if (!contact_type) {
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

  load_switch->connect_to(new Repeat<bool, bool>(10000))
      ->connect_to(new SKOutputBool(sk_path, config_path_sk_output));

  // Setup a ValueListener so changing the value with a SK plugin can cause
  // the relay to turn on or off
  String sk_path2 = "electrical.commands.switch."
      + config_path_sk_output.substring(8, config_path_sk_output.length());  

  auto* sk_listener2 = new SKValueListener<String>(sk_path2);
    sk_listener2->connect_to(controller->truthy_string_consumer_);  

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
  // can cause a reboot sequence for manual network control
  auto* put_reboot_listener = new BoolSKPutRequestListener(reboot_path);
    put_reboot_listener->connect_to(new LambdaConsumer<bool>(
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
                    //->set_wifi_client("My WiFi SSID", "my_wifi_password")
                    //->set_wifi_access_point("My AP SSID", "my_ap_password")
                    //->set_sk_server("192.168.10.3", 80)
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


    while(true)
    {
    loop();
    }

}

void loop() { event_loop()->tick();}
