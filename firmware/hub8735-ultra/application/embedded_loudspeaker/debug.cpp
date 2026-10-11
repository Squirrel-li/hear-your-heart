#include "debug.h"

#include <FreeRTOS.h>
#include <semphr.h>
#include <stdarg.h>
#include <stdio.h>

namespace {

constexpr size_t LOG_LINE_MAX = 192;
constexpr char LEVEL_CHARS[] = {'E', 'W', 'I', 'D', 'T'};

SemaphoreHandle_t logMutex = nullptr;
char lineBuf[LOG_LINE_MAX];  // shared under logMutex; keeps task stacks small

} // namespace

void logInit()
{
    if (logMutex == nullptr) logMutex = xSemaphoreCreateMutex();
}

void logWrite(uint8_t level, const char* tag, const char* fmt, ...)
{
    // Before logInit() (or if it failed) there is no lock; a garbled line beats no line.
    const bool locked = logMutex != nullptr &&
                        xSemaphoreTake(logMutex, pdMS_TO_TICKS(50)) == pdPASS;

    int n = snprintf(lineBuf, sizeof(lineBuf), "[%lu][%c][%s] ",
                     static_cast<unsigned long>(millis()),
                     level < sizeof(LEVEL_CHARS) ? LEVEL_CHARS[level] : '?', tag);
    if (n < 0) n = 0;
    if (static_cast<size_t>(n) < sizeof(lineBuf)) {
        va_list args;
        va_start(args, fmt);
        const int m = vsnprintf(lineBuf + n, sizeof(lineBuf) - n, fmt, args);
        va_end(args);
        if (m > 0) n += m;
    }
    if (static_cast<size_t>(n) > sizeof(lineBuf) - 2) n = sizeof(lineBuf) - 2;  // truncated
    lineBuf[n++] = '\n';

#ifdef DEBUG_MODE
    Serial.write(reinterpret_cast<const uint8_t*>(lineBuf), n);
#else
    Serial2.write(reinterpret_cast<const uint8_t*>(lineBuf), n);
#endif

    if (locked) xSemaphoreGive(logMutex);
}
