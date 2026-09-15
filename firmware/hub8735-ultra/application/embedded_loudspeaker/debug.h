#ifndef DEBUG_H
#define DEBUG_H

#include <Arduino.h>

#define DEBUG_MODE

#ifdef DEBUG_MODE
#define DEBUG_PRINTLN(x) Serial.println(x)
#define DEBUG_PRINT(x) Serial.print(x)
#else
#define DEBUG_PRINTLN(x) Serial2.println(x)
#define DEBUG_PRINT(x) Serial2.print(x)
#endif

#endif // DEBUG_H