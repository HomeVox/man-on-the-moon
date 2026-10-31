/*
 * Jarvis - wifi als client (station), met automatisch opnieuw verbinden.
 */
#pragma once

namespace jarvis {

// Start wifi en verbindt; keert direct terug. Opnieuw verbinden gebeurt vanzelf.
void wifiStart(const char* ssid, const char* password);
bool wifiConnected();

}  // namespace jarvis
