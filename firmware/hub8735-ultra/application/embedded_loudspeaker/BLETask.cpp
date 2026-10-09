#include "BLETask.h"


BLEAdvertData foundDevice;
BLEAdvertData targetDevice;
BLEClient* client = nullptr;
BLERemoteService* UartService = nullptr;
BLERemoteCharacteristic* Rx = nullptr;
BLERemoteCharacteristic* Tx = nullptr;

namespace {

constexpr uint32_t BLE_SCAN_MS = 2000;
constexpr uint32_t BLE_CONNECT_TIMEOUT_MS = 3000;
constexpr uint32_t BLE_DISCOVERY_TIMEOUT_MS = 10000;
constexpr uint32_t BLE_RETRY_DELAY_MS = 3000;

volatile bool targetFound = false;
bool clientConfigured = false;
int8_t connID = -1;

// The BLE library deletes the client and its services on disconnect.
void clearRemoteHandles()
{
    client = nullptr;
    UartService = nullptr;
    Rx = nullptr;
    Tx = nullptr;
}

// The display only needs the latest values, so a full queue just drops the update.
void postUi(const UiUpdateElement& update)
{
    if (uiUpdateQueue != nullptr) {
        xQueueSend(uiUpdateQueue, &update, 0);
    }
}

bool sensorConnected()
{
    return connID >= 0 && BLE.connected(connID);
}

void dropConnection(const char* reason)
{
    DEBUG_PRINT("BLE: ");
    DEBUG_PRINTLN(reason);
    if (sensorConnected()) {
        BLE.configConnection()->disconnect(connID);
    }
    connID = -1;
    clearRemoteHandles();
}

bool connectSensor()
{
    clearRemoteHandles();
    connID = -1;
    targetFound = false;

    DEBUG_PRINT("BLE: scanning for ");
    DEBUG_PRINTLN(TARGET_NAME);
    BLE.configScan()->startScan(BLE_SCAN_MS);
    if (!targetFound) {
        DEBUG_PRINTLN("BLE: target not found");
        return false;
    }

    DEBUG_PRINT("BLE: connecting to ");
    DEBUG_PRINTLN(targetDevice.getAddr().str());
    if (!BLE.configConnection()->connect(targetDevice, BLE_CONNECT_TIMEOUT_MS)) {
        DEBUG_PRINTLN("BLE: connect request failed");
        return false;
    }
    const uint32_t connectStart = millis();
    while ((uint32_t)(millis() - connectStart) < BLE_CONNECT_TIMEOUT_MS) {
        connID = BLE.configConnection()->getConnId(targetDevice);
        if (sensorConnected()) break;
        connID = -1;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (connID < 0) {
        DEBUG_PRINTLN("BLE: connect timeout");
        return false;
    }
    DEBUG_PRINT("BLE: connected, connID=");
    DEBUG_PRINTLN(static_cast<int>(connID));

    if (!clientConfigured) {
        BLE.configClient();
        clientConfigured = true;
    }
    client = BLE.addClient(connID);
    if (client == nullptr) {
        dropConnection("add client failed");
        return false;
    }
    client->discoverServices();
    DEBUG_PRINT("BLE: discovering services");
    const uint32_t discoveryStart = millis();
    while (!client->discoveryDone()) {
        if (!sensorConnected()) {
            DEBUG_PRINT("\n");
            dropConnection("disconnected during discovery");
            return false;
        }
        if ((uint32_t)(millis() - discoveryStart) >= BLE_DISCOVERY_TIMEOUT_MS) {
            DEBUG_PRINT("\n");
            dropConnection("service discovery timeout");
            return false;
        }
        DEBUG_PRINT(".");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    DEBUG_PRINT("\n");

    UartService = client->getService(UART_SERVICE_UUID);
    if (UartService == nullptr) {
        dropConnection("UART service not found");
        return false;
    }
    Tx = UartService->getCharacteristic(CHARACTERISTIC_UUID_TX);
    if (Tx == nullptr) {
        dropConnection("TX characteristic not found");
        return false;
    }
    DEBUG_PRINTLN("TX characteristic found");
    Tx->setBufferLen(STRING_BUF_SIZE);
    Tx->setNotifyCallback(notificationCB);
    Tx->enableNotifyIndicate();
    Rx = UartService->getCharacteristic(CHARACTERISTIC_UUID_RX);
    if (Rx != nullptr) {
        DEBUG_PRINTLN("RX characteristic found");
        Rx->setBufferLen(STRING_BUF_SIZE);
    }
    DEBUG_PRINTLN("BLE: sensor connected, notifications enabled");
    return true;
}

} // namespace

void scanCB(T_LE_CB_DATA* p_data)
{
    foundDevice.parseScanInfo(p_data);
    if (foundDevice.hasName()) {
        if (foundDevice.getName() == String(TARGET_NAME)) {
            DEBUG_PRINT("Found target BLE Device at address ");
            DEBUG_PRINTLN(foundDevice.getAddr().str());
            targetDevice = foundDevice;
            targetFound = true;
        }
    }
}

void notificationCB(BLERemoteCharacteristic* chr, uint8_t* data, uint16_t len)
{
    // Create a buffer to store the received data
    BleFrameElement msgElement;
    msgElement.hub_rx_time_ms = millis();
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
    DEBUG_PRINTLN("BLE initialized successfully");

    bool linkUp = connectSensor();
    uint32_t lastAttemptMs = millis();
    uint32_t lastUiBleMs = 0;
    bool lastUiBle = !linkUp;
	while (true) {
        if (linkUp && !sensorConnected()) {
            linkUp = false;
            connID = -1;
            clearRemoteHandles();
            lastAttemptMs = millis();
            DEBUG_PRINTLN("BLE: sensor disconnected, will reconnect");
        }
        if (!linkUp &&
            (uint32_t)(millis() - lastAttemptMs) >= BLE_RETRY_DELAY_MS) {
            DEBUG_PRINTLN("BLE: reconnecting");
            linkUp = connectSensor();
            lastAttemptMs = millis();
            if (!linkUp) {
                DEBUG_PRINT("BLE: reconnect failed, retry in ");
                DEBUG_PRINT(BLE_RETRY_DELAY_MS);
                DEBUG_PRINTLN(" ms");
            }
        }

        // Resent every second so a dropped update cannot leave a stale link state on screen.
        if (linkUp != lastUiBle ||
            (uint32_t)(millis() - lastUiBleMs) >= 1000u) {
            UiUpdateElement bleUpdate{};
            bleUpdate.type = UiUpdateType::Ble;
            bleUpdate.timestampMs = millis();
            bleUpdate.bleConnected = linkUp ? 1 : 0;
            postUi(bleUpdate);
            lastUiBle = linkUp;
            lastUiBleMs = millis();
        }

        BleFrameElement msgElement;
        if (xQueueReceive(bleRxFrameQueue, &msgElement, 0) == pdPASS) {
            if (msgElement.length < 9) {
                DEBUG_PRINTLN("Error: Received BLE frame is too short");
                continue; // Skip processing for this frame
            }
            uint32_t sensor_time_ms =
                (static_cast<uint32_t>(msgElement.data[0]) << 24) |
                (static_cast<uint32_t>(msgElement.data[1]) << 16) |
                (static_cast<uint32_t>(msgElement.data[2]) << 8) |
                static_cast<uint32_t>(msgElement.data[3]);
            uint8_t msgType = msgElement.data[4];
            uint32_t sequence =
                (static_cast<uint32_t>(msgElement.data[5]) << 24) |
                (static_cast<uint32_t>(msgElement.data[6]) << 16) |
                (static_cast<uint32_t>(msgElement.data[7]) << 8) |
                static_cast<uint32_t>(msgElement.data[8]);
            DEBUG_PRINT("BLETask <- BLE callback: msgType=");
            DEBUG_PRINT(msgType);
            DEBUG_PRINT(" sequence=");
            DEBUG_PRINTLN(sequence);
            // DEBUG_PRINT("BLETask: Received BLE frame with msgType: ");
            // DEBUG_PRINTLN(msgType);
            // DEBUG_PRINT("Receive timestamp: ");
            // DEBUG_PRINTLN(msgElement.hub_rx_time_ms);
            // DEBUG_PRINT("Device timestamp: ");
            // DEBUG_PRINTLN(sensor_time_ms);
            // DEBUG_PRINT("Sequence: ");
            // DEBUG_PRINTLN(sequence);
            
            if (msgType == 0x01) { // PPG sensor
                if (msgElement.length < 20) {
                    DEBUG_PRINTLN("Error: PPG sensor data length is less than expected");
                    continue; // Skip processing for this frame
                }
                PpgSensorElement ppgElement;
                ppgElement.hub_rx_time_ms = msgElement.hub_rx_time_ms;
                ppgElement.sensor_time_ms = sensor_time_ms;
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
                UiUpdateElement ppgUpdate{};
                ppgUpdate.type = UiUpdateType::Sensor;
                ppgUpdate.timestampMs = ppgElement.hub_rx_time_ms;
                ppgUpdate.heartRateBpm = ppgElement.heartRateBpm;
                ppgUpdate.spo2 = ppgElement.SpO2;
                ppgUpdate.sensorStatus = ppgElement.status;
                postUi(ppgUpdate);
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
            } else if (msgType == 0x02) { // G sensor
                if (msgElement.length < 21) {
                    DEBUG_PRINTLN("Error: G sensor data length is less than expected");
                    continue; // Skip processing for this frame
                }
                GSensorElement gSensorElement;
                gSensorElement.hub_rx_time_ms = msgElement.hub_rx_time_ms;
                gSensorElement.sensor_time_ms = sensor_time_ms;
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
                UiUpdateElement gUpdate{};
                gUpdate.type = UiUpdateType::GSensor;
                gUpdate.timestampMs = gSensorElement.hub_rx_time_ms;
                gUpdate.acc[0] = gSensorElement.acc_x;
                gUpdate.acc[1] = gSensorElement.acc_y;
                gUpdate.acc[2] = gSensorElement.acc_z;
                gUpdate.gyro[0] = gSensorElement.gyro_x;
                gUpdate.gyro[1] = gSensorElement.gyro_y;
                gUpdate.gyro[2] = gSensorElement.gyro_z;
                postUi(gUpdate);
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
                DEBUG_PRINTLN("Unknown msgType received");
                continue; // Skip processing for unknown msgType
            }
        }
		vTaskDelay(pdMS_TO_TICKS(100));
	}
}