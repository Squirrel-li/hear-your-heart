#ifndef BLE_TASK_H
#define BLE_TASK_H

#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include "BLEDevice.h"

#include "debug.h"
#include "globalData.h"

#define TARGET_NAME "BLE_sensor_ITAP"

#define UART_SERVICE_UUID      "FFF0"
#define CHARACTERISTIC_UUID_TX "FFF1"
#define CHARACTERISTIC_UUID_RX "FFF2"

// #define UART_SERVICE_UUID      "524CACC0-3C17-D293-8E48-14FE2E4DA212"
// #define CHARACTERISTIC_UUID_TX "D002"
// #define CHARACTERISTIC_UUID_RX "D001"
#define STRING_BUF_SIZE 100


extern BLEAdvertData foundDevice;
extern BLEAdvertData targetDevice;
extern BLEClient* client;
extern BLERemoteService* UartService;
extern BLERemoteCharacteristic* Rx;
extern BLERemoteCharacteristic* Tx;

void scanCB(T_LE_CB_DATA* p_data);

void notificationCB(BLERemoteCharacteristic* chr, uint8_t* data, uint16_t len);

void BLETask(void* parameter);

#endif // BLE_TASK_H