// Copy this file to secrets.h (same folder) and fill in your own values. secrets.h is ignored by git.
#pragma once

#define WIFI_SSID     "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"

// Home Assistant REST API: only needed when MQTT_USER is left empty (fallback that reads two HA helpers).
// With MQTT filled in you can keep these placeholders.
#define HA_HOST  "homeassistant.local"
#define HA_PORT  8123
#define HA_TOKEN "your-long-lived-access-token"
