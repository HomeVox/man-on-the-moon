// Copy this file to mqtt_secrets.h (same folder) and fill in your MQTT broker. mqtt_secrets.h is ignored by git.
// The StopWatch shows up in Home Assistant by itself through MQTT discovery (device "Stopwatch").
#pragma once

#define MQTT_HOST "homeassistant.local"
#define MQTT_PORT 1883
#define MQTT_USER "your-mqtt-user"
#define MQTT_PASS "your-mqtt-password"
