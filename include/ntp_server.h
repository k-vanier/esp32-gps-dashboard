// Serveur NTP (UDP/123) stratum 1, référence GPS.
#pragma once

#include <Arduino.h>

void ntpServerBegin();
void ntpServerLoop();  // à appeler le plus souvent possible dans loop()
uint32_t ntpRequestCount();
