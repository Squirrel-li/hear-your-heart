/* Acquisition-only build: share the existing UART/BM7701/I2C drivers.
 * No local HR algorithm. See ../README.md for the recording workflow. */
#define ITAP_CAPTURE 1
#include "../../ble_sensor/source/main.c"
