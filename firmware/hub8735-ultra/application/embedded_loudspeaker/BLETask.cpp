#include "BLETask.h"


BLEAdvertData foundDevice;
BLEAdvertData targetDevice;
BLEClient* client = nullptr;
BLERemoteService* UartService = nullptr;
BLERemoteCharacteristic* Rx = nullptr;
BLERemoteCharacteristic* Tx = nullptr;

void scanCB(T_LE_CB_DATA* p_data)
{
    foundDevice.parseScanInfo(p_data);
    if (foundDevice.hasName()) {
        if (foundDevice.getName() == String(TARGET_NAME)) {
            DEBUG_PRINT("Found target BLE Device at address ");
            DEBUG_PRINTLN(foundDevice.getAddr().str());
            targetDevice = foundDevice;
        }
    }
}

void notificationCB(BLERemoteCharacteristic* chr, uint8_t* data, uint16_t len)
{
    // Create a buffer to store the received data
    BleFrameElement msgElement;
    msgElement.receive_timestamp = millis();
    msgElement.length = len;
    if (len > BLE_FRAME_MAX) {
        DEBUG_PRINTLN("Error: Received BLE notification data length exceeds buffer size");
        return; // Skip processing for this notification
    }
    memcpy(msgElement.data, data, len);
    if (xQueueSend(bleRxFrameQueue, &msgElement, 0) == pdPASS) {
        DEBUG_PRINT("BLE callback -> BLETask: queued frame, len=");
        DEBUG_PRINTLN(msgElement.length);
    } else {
        DEBUG_PRINTLN("BLE callback -> BLETask: frame queue full");
    }
    DEBUG_PRINT("\n\nNotification received for chr UUID: ");
    DEBUG_PRINTLN(chr->getUUID().str());
    DEBUG_PRINT("Received data: ");
    #ifdef DEBUG_MODE
    for (int i = 0; i < len; i++) {
        Serial.print(msgElement.data[i], HEX);
        DEBUG_PRINT(" ");
    }
    DEBUG_PRINT("\n");
    #endif
}

void BLETask(void* parameter) {
	// 初始化
    // BLE

    BLE.init();
    BLE.setScanCallback(scanCB);
    BLE.beginCentral(1);
	
    BLE.configScan()->startScan(2000);
    BLE.configConnection()->connect(targetDevice, 2000);

	vTaskDelay(2000);
    int8_t connID = BLE.configConnection()->getConnId(targetDevice);
	if (!BLE.connected(connID)) {
        DEBUG_PRINTLN("BLE not connected");
    } else {
        BLE.configClient();
        client = BLE.addClient(connID);
        client->discoverServices();
        DEBUG_PRINT("Discovering services of connected device");
        do {
            DEBUG_PRINT(".");
            delay(1000);
        } while (!(client->discoveryDone()));
        DEBUG_PRINT("\n");

        UartService = client->getService(UART_SERVICE_UUID);
        if (UartService != nullptr) {
            Tx = UartService->getCharacteristic(CHARACTERISTIC_UUID_TX);
            if (Tx != nullptr) {
                DEBUG_PRINTLN("TX characteristic found");
                Tx->setBufferLen(STRING_BUF_SIZE);
                Tx->setNotifyCallback(notificationCB);
                Tx->enableNotifyIndicate();
            }
            Rx = UartService->getCharacteristic(CHARACTERISTIC_UUID_RX);
            if (Rx != nullptr) {
                DEBUG_PRINTLN("RX characteristic found");
                Rx->setBufferLen(STRING_BUF_SIZE);
            }
        }
    }
    DEBUG_PRINTLN("BLE initialized successfully");
	while (true) {
        BleFrameElement msgElement;
        if (xQueueReceive(bleRxFrameQueue, &msgElement, 0) == pdPASS) {
            if (msgElement.length < 9) {
                DEBUG_PRINTLN("Error: Received BLE frame is too short");
                continue; // Skip processing for this frame
            }
            uint32_t device_timestamp =
                (static_cast<uint32_t>(msgElement.data[0]) << 24) |
                (static_cast<uint32_t>(msgElement.data[1]) << 16) |
                (static_cast<uint32_t>(msgElement.data[2]) << 8) |
                static_cast<uint32_t>(msgElement.data[3]);
            uint8_t sensorId = msgElement.data[4];
            uint32_t sequence =
                (static_cast<uint32_t>(msgElement.data[5]) << 24) |
                (static_cast<uint32_t>(msgElement.data[6]) << 16) |
                (static_cast<uint32_t>(msgElement.data[7]) << 8) |
                static_cast<uint32_t>(msgElement.data[8]);
            DEBUG_PRINT("BLETask <- BLE callback: sensorId=");
            DEBUG_PRINT(sensorId);
            DEBUG_PRINT(" sequence=");
            DEBUG_PRINTLN(sequence);
            // DEBUG_PRINT("BLETask: Received BLE frame with sensor ID: ");
            // DEBUG_PRINTLN(sensorId);
            // DEBUG_PRINT("Receive timestamp: ");
            // DEBUG_PRINTLN(msgElement.receive_timestamp);
            // DEBUG_PRINT("Device timestamp: ");
            // DEBUG_PRINTLN(device_timestamp);
            // DEBUG_PRINT("Sequence: ");
            // DEBUG_PRINTLN(sequence);
            
            if (sensorId == 0x01) { // PPG sensor
                if (msgElement.length < 20) {
                    DEBUG_PRINTLN("Error: PPG sensor data length is less than expected");
                    continue; // Skip processing for this frame
                }
                PpgSensorElement ppgElement;
                ppgElement.receive_timestamp = msgElement.receive_timestamp;
                ppgElement.device_timestamp = device_timestamp;
                ppgElement.sequence = sequence;
                ppgElement.status = msgElement.data[9];
                ppgElement.SpO2 = msgElement.data[10];
                ppgElement.heartRateBpm = msgElement.data[11];
                ppgElement.bpi = msgElement.data[12];
                ppgElement.hrv = msgElement.data[13];
                ppgElement.pulseAmplitude = msgElement.data[14];
                ppgElement.systoleDuration = msgElement.data[15];
                ppgElement.ppg1 = (msgElement.data[16] << 8) | msgElement.data[17];
                ppgElement.ppg2 = (msgElement.data[18] << 8) | msgElement.data[19];
                if (xQueueSend(ppgSensorQueue, &ppgElement, 0) == pdPASS) {
                    DEBUG_PRINT("BLETask -> analysisTask: PPG HR=");
                    DEBUG_PRINT(ppgElement.heartRateBpm);
                    DEBUG_PRINT(" status=");
                    DEBUG_PRINT(ppgElement.status);
                    DEBUG_PRINT(" sequence=");
                    DEBUG_PRINTLN(ppgElement.sequence);
                } else {
                    DEBUG_PRINTLN("BLETask -> analysisTask: PPG queue full");
                }
                // DEBUG_PRINTLN("PPG sensor data received");
                // DEBUG_PRINT("Status: ");
                // DEBUG_PRINTLN(ppgElement.status);
                // DEBUG_PRINT("SpO2: ");
                // DEBUG_PRINTLN(ppgElement.SpO2);
                // DEBUG_PRINT("Heart Rate: ");
                // DEBUG_PRINTLN(ppgElement.heartRateBpm);
                // DEBUG_PRINT("BPI: ");
                // DEBUG_PRINTLN(ppgElement.bpi);
                // DEBUG_PRINT("HRV: ");
                // DEBUG_PRINTLN(ppgElement.hrv);
                // DEBUG_PRINT("Pulse Amplitude: ");
                // DEBUG_PRINTLN(ppgElement.pulseAmplitude);
                // DEBUG_PRINT("Systole Duration: ");
                // DEBUG_PRINTLN(ppgElement.systoleDuration);
                // DEBUG_PRINT("PPG1: ");
                // DEBUG_PRINTLN(ppgElement.ppg1);
                // DEBUG_PRINT("PPG2: ");
                // DEBUG_PRINTLN(ppgElement.ppg2);
            } else if (sensorId == 0x02) { // G sensor
                if (msgElement.length < 21) {
                    DEBUG_PRINTLN("Error: G sensor data length is less than expected");
                    continue; // Skip processing for this frame
                }
                GSensorElement gSensorElement;
                gSensorElement.receive_timestamp = msgElement.receive_timestamp;
                gSensorElement.device_timestamp = device_timestamp;
                gSensorElement.sequence = sequence;
                gSensorElement.acc_x = static_cast<int16_t>((static_cast<uint16_t>(msgElement.data[9]) << 8) | msgElement.data[10]);
                gSensorElement.acc_y = static_cast<int16_t>((static_cast<uint16_t>(msgElement.data[11]) << 8) | msgElement.data[12]);
                gSensorElement.acc_z = static_cast<int16_t>((static_cast<uint16_t>(msgElement.data[13]) << 8) | msgElement.data[14]);
                gSensorElement.gyro_x = static_cast<int16_t>((static_cast<uint16_t>(msgElement.data[15]) << 8) | msgElement.data[16]);
                gSensorElement.gyro_y = static_cast<int16_t>((static_cast<uint16_t>(msgElement.data[17]) << 8) | msgElement.data[18]);
                gSensorElement.gyro_z = static_cast<int16_t>((static_cast<uint16_t>(msgElement.data[19]) << 8) | msgElement.data[20]);
                if (xQueueSend(gSensorQueue, &gSensorElement, 0) == pdPASS) {
                    DEBUG_PRINT("BLETask -> analysisTask: G sensor sequence=");
                    DEBUG_PRINTLN(gSensorElement.sequence);
                } else {
                    DEBUG_PRINTLN("BLETask -> analysisTask: G sensor queue full");
                }
                DEBUG_PRINTLN("G sensor data received");
                DEBUG_PRINT("Acc X: ");
                DEBUG_PRINTLN(gSensorElement.acc_x);
                DEBUG_PRINT("Acc Y: ");
                DEBUG_PRINTLN(gSensorElement.acc_y);
                DEBUG_PRINT("Acc Z: ");
                DEBUG_PRINTLN(gSensorElement.acc_z);
                DEBUG_PRINT("Gyro X: ");
                DEBUG_PRINTLN(gSensorElement.gyro_x);
                DEBUG_PRINT("Gyro Y: ");
                DEBUG_PRINTLN(gSensorElement.gyro_y);
                DEBUG_PRINT("Gyro Z: ");
                DEBUG_PRINTLN(gSensorElement.gyro_z);
            } else {
                DEBUG_PRINTLN("Unknown sensor ID received");
                continue; // Skip processing for unknown sensor ID
            }
        }
		vTaskDelay(pdMS_TO_TICKS(100));
	}
}