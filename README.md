# SensESP Project Template

This repository is specific to the Waveshare 6 relay module.
https://www.waveshare.com/wiki/ESP32-S3-Relay-6CH easily available at Amazon.

It uses the SensESP framework to connect to a SignalK server.
On startup you should be able to connect to
the WiFi access point with the same name as the device. The password is `thisisfine`. From there you can then configure it to work with your network and SignalK server.

Comprehensive documentation for SensESP, including how to get started with your own project, is available at the [SensESP documentation site](https://signalk.org/SensESP/).

This allows you to control the six relays using PUT requests from any device on your network it works extremely well with the SKipper app https://www.skipperapp.net/

It can also be used for network monitoring and perform rebooting of stuck devices. Using a SignalK plugin to monitor network devices and changing of a value on the SignalK server that this device listens to, will initiate the reboot sequence of the relay. Because the power on and off are locally controlled on this device things like modems and routers that are critical to connectivity can be safely restarted remotely or automatically.
