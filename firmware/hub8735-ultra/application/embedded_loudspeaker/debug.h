#ifndef DEBUG_H
#define DEBUG_H

#include <Arduino.h>

// Logging: one line per call, "[ms][L][TAG] message". The whole line is formatted
// first and written in one piece under a mutex, so tasks never interleave.
//
// Levels (a call is compiled out when above LOG_LEVEL):
//   E error  - something failed and the feature is degraded
//   W warn   - unexpected but handled (drop, retry, fallback)
//   I info   - state changes and user-visible actions
//   D debug  - per-step detail (selection, analysis steps, queue traffic)
//   T trace  - high-rate data (every sensor frame, every redraw)
#define LOG_LEVEL_ERROR 0
#define LOG_LEVEL_WARN  1
#define LOG_LEVEL_INFO  2
#define LOG_LEVEL_DEBUG 3
#define LOG_LEVEL_TRACE 4

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_DEBUG
#endif

// Output port: Serial (LOG UART, TX = D17, RX = D16).

// Call before any task starts (creates the mutex). Serial must already be begun.
void logInit();
void logWrite(uint8_t level, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define LOG_AT(level, tag, ...) \
    do { if ((level) <= LOG_LEVEL) logWrite((level), (tag), __VA_ARGS__); } while (0)

#define LOGE(tag, ...) LOG_AT(LOG_LEVEL_ERROR, tag, __VA_ARGS__)
#define LOGW(tag, ...) LOG_AT(LOG_LEVEL_WARN,  tag, __VA_ARGS__)
#define LOGI(tag, ...) LOG_AT(LOG_LEVEL_INFO,  tag, __VA_ARGS__)
#define LOGD(tag, ...) LOG_AT(LOG_LEVEL_DEBUG, tag, __VA_ARGS__)
#define LOGT(tag, ...) LOG_AT(LOG_LEVEL_TRACE, tag, __VA_ARGS__)

#endif // DEBUG_H
