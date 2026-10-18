#ifndef BLE_TASK_H
#define BLE_TASK_H

#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include "BLEDevice.h"

#include "debug.h"
#include "globalData.h"

#define CHEST_NAME "BLE_sensor_ITAP"
#define WATCH_NAME "BLE_watch_ITAP"

#define STRING_BUF_SIZE 100

void scanCB(T_LE_CB_DATA* p_data);

void notificationCB(BLERemoteCharacteristic* chr, uint8_t* data, uint16_t len);

void BLETask(void* parameter);

#endif // BLE_TASK_H