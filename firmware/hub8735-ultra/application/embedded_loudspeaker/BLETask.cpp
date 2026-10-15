#include "BLETask.h"


BLEAdvertData foundDevice;
BLEAdvertData targetDevice;
BLEClient* client = nullptr;
BLERemoteService* UartService = nullptr;
BLERemoteCharacteristic* Rx = nullptr;
BLERemoteCharacteristic* Tx = nullptr;

void scanCB(T_LE_CB_DATA* p_data); // defined below, outside the anonymous namespace

namespace {

constexpr const char* TAG = "BLE";
constexpr uint32_t BLE_SCAN_MS = 2000;
constexpr uint32_t BLE_CONNECT_TIMEOUT_MS = 3000;
constexpr uint32_t BLE_DISCOVERY_TIMEOUT_MS = 10000;
constexpr uint32_t BLE_RETRY_DELAY_MS = 3000;

// 依序嘗試的感測端 UUID 組合：先 FFF0，找不到再試 524CACC0。
struct UartUuidSet {
    const char* service;
    const char* tx;  // 感測端 notify
    const char* rx;
};
const UartUuidSet UART_UUID_SETS[] = {
    {"FFF0", "FFF1", "FFF2"},
    {"524CACC0-3C17-D293-8E48-14FE2E4DA212", "D002", "D001"},
};

volatile bool targetFound = false;
bool clientConfigured = false;
int8_t connID = -1;
uint32_t connectAttempts = 0;

// Written by notificationCB, read once a second by BLETask for the stats line.
volatile uint32_t cbFrames = 0;
volatile uint32_t cbQueueFull = 0;
volatile uint32_t cbOversize = 0;
volatile uint32_t cbDuplicates = 0;

// ponytail: the BLE library registers one more GATT client on every BLE.addClient() and never
// unregisters it, so after N connections each notification reaches the callback N times (and the
// 4th registration fails, BLE_CENTRAL_APP_MAX_LINKS = 3). Identical back-to-back frames are
// dropped here (real frames never repeat: they carry a sequence number), and the stack is
// restarted before the registrations run out. Remove both once the library can unregister clients.
uint8_t lastFrame[BLE_FRAME_MAX];
volatile uint16_t lastFrameLen = 0;
uint8_t clientRegistrations = 0;

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

int16_t readInt16BE(const uint8_t* p)
{
    return static_cast<int16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

void handleAcc(const BleFrameElement& frame, uint32_t sensorTimeMs, uint32_t sequence, const uint8_t* xyz,
               uint32_t& statsAcc)
{
    AccElement accElement;
    accElement.hub_rx_time_ms = frame.hub_rx_time_ms;
    accElement.sensor_time_ms = sensorTimeMs;
    accElement.sequence = sequence;
    accElement.acc_x = readInt16BE(&xyz[0]);
    accElement.acc_y = readInt16BE(&xyz[2]);
    accElement.acc_z = readInt16BE(&xyz[4]);
    ++statsAcc;
    LOGD(TAG, "Acc sensor_ms=%lu sequence=%lu x=%d y=%d z=%d", static_cast<unsigned long>(sensorTimeMs),
         static_cast<unsigned long>(sequence), accElement.acc_x, accElement.acc_y, accElement.acc_z);
    if (xQueueSend(accSensorQueue, &accElement, 0) != pdPASS) {
        LOGW(TAG, "Acc queue full, sample dropped (sequence=%lu)",
             static_cast<unsigned long>(accElement.sequence));
    }
    UiUpdateElement accUpdate{};
    accUpdate.type = UiUpdateType::Acc;
    accUpdate.timestampMs = accElement.hub_rx_time_ms;
    accUpdate.acc[0] = accElement.acc_x;
    accUpdate.acc[1] = accElement.acc_y;
    accUpdate.acc[2] = accElement.acc_z;
    postUi(accUpdate);
}

void handleGyro(const BleFrameElement& frame, uint32_t sensorTimeMs, uint32_t sequence, const uint8_t* xyz,
                uint32_t& statsGyro)
{
    GyroElement gyroElement;
    gyroElement.hub_rx_time_ms = frame.hub_rx_time_ms;
    gyroElement.sensor_time_ms = sensorTimeMs;
    gyroElement.sequence = sequence;
    gyroElement.gyro_x = readInt16BE(&xyz[0]);
    gyroElement.gyro_y = readInt16BE(&xyz[2]);
    gyroElement.gyro_z = readInt16BE(&xyz[4]);
    ++statsGyro;
    LOGD(TAG, "Gyro sensor_ms=%lu sequence=%lu x=%d y=%d z=%d", static_cast<unsigned long>(sensorTimeMs),
         static_cast<unsigned long>(sequence), gyroElement.gyro_x, gyroElement.gyro_y, gyroElement.gyro_z);
    if (xQueueSend(gyroSensorQueue, &gyroElement, 0) != pdPASS) {
        LOGW(TAG, "Gyro queue full, sample dropped (sequence=%lu)",
             static_cast<unsigned long>(gyroElement.sequence));
    }
    UiUpdateElement gyroUpdate{};
    gyroUpdate.type = UiUpdateType::Gyro;
    gyroUpdate.timestampMs = gyroElement.hub_rx_time_ms;
    gyroUpdate.gyro[0] = gyroElement.gyro_x;
    gyroUpdate.gyro[1] = gyroElement.gyro_y;
    gyroUpdate.gyro[2] = gyroElement.gyro_z;
    postUi(gyroUpdate);
}

bool sensorConnected()
{
    return connID >= 0 && BLE.connected(connID);
}

void dropConnection(const char* reason)
{
    LOGW(TAG, "dropping connection: %s", reason);
    if (sensorConnected()) {
        BLE.configConnection()->disconnect(connID);
    }
    connID = -1;
    clearRemoteHandles();
}

void restartBleStack()
{
    LOGI(TAG, "restarting BLE stack to release client registrations (%u used)",
         static_cast<unsigned>(clientRegistrations));
    clearRemoteHandles();
    BLE.deinit();
    BLE.init();
    BLE.setScanCallback(scanCB);
    BLE.beginCentral(1);
    clientConfigured = false;
    clientRegistrations = 0;
}

bool connectSensor()
{
    if (clientRegistrations >= BLE_CENTRAL_APP_MAX_LINKS) restartBleStack();
    lastFrameLen = 0;
    clearRemoteHandles();
    connID = -1;
    targetFound = false;

    ++connectAttempts;
    LOGI(TAG, "attempt #%lu: scanning for '%s' (%lu ms)",
         static_cast<unsigned long>(connectAttempts), TARGET_NAME,
         static_cast<unsigned long>(BLE_SCAN_MS));
    // startScan(ms) always waits the full duration; stop as soon as the target is seen.
    BLE.configScan()->startScan();
    for (uint32_t waited = 0; waited < BLE_SCAN_MS && !targetFound; waited += 20) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    BLE.configScan()->stopScan();
    vTaskDelay(pdMS_TO_TICKS(100));  // same settle time the library's timed scan leaves
    if (!targetFound) {
        LOGW(TAG, "target '%s' not found", TARGET_NAME);
        return false;
    }

    LOGI(TAG, "connecting to %s (timeout %lu ms)", targetDevice.getAddr().str(),
         static_cast<unsigned long>(BLE_CONNECT_TIMEOUT_MS));
    if (!BLE.configConnection()->connect(targetDevice, BLE_CONNECT_TIMEOUT_MS)) {
        LOGW(TAG, "connect request failed");
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
        LOGW(TAG, "connect timeout after %lu ms", static_cast<unsigned long>(BLE_CONNECT_TIMEOUT_MS));
        return false;
    }
    LOGI(TAG, "connected, connID=%d (%lu ms)", static_cast<int>(connID),
         static_cast<unsigned long>(millis() - connectStart));

    if (!clientConfigured) {
        BLE.configClient();
        clientConfigured = true;
    }
    client = BLE.addClient(connID);
    if (client == nullptr) {
        dropConnection("add client failed");
        return false;
    }
    ++clientRegistrations;
    client->discoverServices();
    LOGI(TAG, "discovering services (timeout %lu ms)", static_cast<unsigned long>(BLE_DISCOVERY_TIMEOUT_MS));
    const uint32_t discoveryStart = millis();
    while (!client->discoveryDone()) {
        if (!sensorConnected()) {
            dropConnection("disconnected during discovery");
            return false;
        }
        if ((uint32_t)(millis() - discoveryStart) >= BLE_DISCOVERY_TIMEOUT_MS) {
            dropConnection("service discovery timeout");
            return false;
        }
        LOGT(TAG, "discovery waiting, %lu ms elapsed", static_cast<unsigned long>(millis() - discoveryStart));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    LOGI(TAG, "service discovery done in %lu ms", static_cast<unsigned long>(millis() - discoveryStart));

    const UartUuidSet* uuids = nullptr;
    for (const UartUuidSet& set : UART_UUID_SETS) {
        UartService = client->getService(set.service);
        if (UartService == nullptr) continue;
        Tx = UartService->getCharacteristic(set.tx);
        if (Tx != nullptr) {
            uuids = &set;
            break;
        }
        LOGW(TAG, "TX characteristic %s not found in service %s", set.tx, set.service);
    }
    if (uuids == nullptr) {
        dropConnection("UART service/TX characteristic not found");
        return false;
    }
    LOGI(TAG, "using service %s (TX=%s RX=%s)", uuids->service, uuids->tx, uuids->rx);
    Tx->setBufferLen(STRING_BUF_SIZE);
    Tx->setNotifyCallback(notificationCB);
    Tx->enableNotifyIndicate();
    Rx = UartService->getCharacteristic(uuids->rx);
    if (Rx != nullptr) {
        LOGD(TAG, "RX characteristic found");
        Rx->setBufferLen(STRING_BUF_SIZE);
    } else {
        LOGW(TAG, "RX characteristic %s not found (commands to sensor unavailable)", uuids->rx);
    }
    LOGI(TAG, "sensor connected, notifications enabled");
    return true;
}

} // namespace

void scanCB(T_LE_CB_DATA* p_data)
{
    foundDevice.parseScanInfo(p_data);
    if (foundDevice.hasName()) {
        if (foundDevice.getName() == String(TARGET_NAME)) {
            // Every advertisement packet lands here; log once per scan window.
            if (!targetFound) LOGI("BLE", "scan: found target at %s", foundDevice.getAddr().str());
            targetDevice = foundDevice;
            targetFound = true;
        }
    }
}

void notificationCB(BLERemoteCharacteristic* chr, uint8_t* data, uint16_t len)
{
    // Runs in the BLE stack's context: count and queue only, no heavy logging.
    BleFrameElement msgElement;
    msgElement.hub_rx_time_ms = millis();
    msgElement.length = len;
    if (len > BLE_FRAME_MAX) {
        ++cbOversize;
        LOGW("BLE", "notification dropped: len=%u > max %u", static_cast<unsigned>(len),
             static_cast<unsigned>(BLE_FRAME_MAX));
        return; // Skip processing for this notification
    }
    if (len == lastFrameLen && memcmp(lastFrame, data, len) == 0) {
        ++cbDuplicates;
        return; // repeated delivery of the same notification
    }
    memcpy(lastFrame, data, len);
    lastFrameLen = len;
    memcpy(msgElement.data, data, len);
    ++cbFrames;
    if (xQueueSend(bleRxFrameQueue, &msgElement, 0) != pdPASS) {
        ++cbQueueFull;
        LOGW("BLE", "frame queue full, frame dropped (len=%u)", static_cast<unsigned>(len));
    }
#if LOG_LEVEL >= LOG_LEVEL_TRACE
    char hex[3 * 24 + 1];
    size_t pos = 0;
    for (uint16_t i = 0; i < len && pos + 3 < sizeof(hex); i++) {
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", msgElement.data[i]);
    }
    LOGT("BLE", "notify %s len=%u: %s%s", chr->getUUID().str(), static_cast<unsigned>(len), hex,
         len > 24 ? "..." : "");
#else
    (void)chr;
#endif
}

void BLETask(void* parameter) {
	// 初始化
    // BLE

    BLE.init();
    BLE.setScanCallback(scanCB);
    BLE.beginCentral(1);
    LOGI(TAG, "BLE stack initialized (central, 1 connection)");

    bool linkUp = connectSensor();
    uint32_t lastAttemptMs = millis();
    uint32_t lastStatsMs = millis();
    uint32_t statsFrames = 0, statsQueueFull = 0;
    uint32_t statsPpg = 0, statsAcc = 0, statsGyro = 0, statsBad = 0;
    uint32_t lastUiBleMs = 0;
    bool lastUiBle = !linkUp;
	while (true) {
        if (linkUp && !sensorConnected()) {
            linkUp = false;
            connID = -1;
            clearRemoteHandles();
            lastAttemptMs = millis();
            LOGW(TAG, "sensor disconnected, reconnect in %lu ms",
                 static_cast<unsigned long>(BLE_RETRY_DELAY_MS));
        }
        if (!linkUp &&
            (uint32_t)(millis() - lastAttemptMs) >= BLE_RETRY_DELAY_MS) {
            LOGI(TAG, "reconnecting");
            linkUp = connectSensor();
            lastAttemptMs = millis();
            if (!linkUp) {
                LOGW(TAG, "reconnect failed, retry in %lu ms", static_cast<unsigned long>(BLE_RETRY_DELAY_MS));
            }
        }

        // Resent every second so a dropped update cannot leave a stale link state on screen.
        if (linkUp != lastUiBle) {
            LOGI(TAG, "link state -> %s", linkUp ? "up" : "down");
        }
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

        if ((uint32_t)(millis() - lastStatsMs) >= 5000u) {
            const uint32_t frames = cbFrames, full = cbQueueFull;
            LOGD(TAG, "5s stats: link=%s rx=%lu (queue full %lu) PPG=%lu Acc=%lu Gyro=%lu bad=%lu, "
                      "oversize=%lu dup=%lu",
                 linkUp ? "up" : "down",
                 static_cast<unsigned long>(frames - statsFrames),
                 static_cast<unsigned long>(full - statsQueueFull),
                 static_cast<unsigned long>(statsPpg), static_cast<unsigned long>(statsAcc),
                 static_cast<unsigned long>(statsGyro), static_cast<unsigned long>(statsBad),
                 static_cast<unsigned long>(cbOversize), static_cast<unsigned long>(cbDuplicates));
            statsFrames = frames;
            statsQueueFull = full;
            statsPpg = statsAcc = statsGyro = statsBad = 0;
            lastStatsMs = millis();
        }

        BleFrameElement msgElement;
        if (xQueueReceive(bleRxFrameQueue, &msgElement, 0) == pdPASS) {
            if (msgElement.length >= 5 && (msgElement.data[4] & 0x0F) == 0x00) { // cmd: reserved, payload not defined yet
                LOGD(TAG, "cmd frame ignored (len=%u)", static_cast<unsigned>(msgElement.length));
                continue;
            }
            if (msgElement.length < 9) {
                LOGW(TAG, "frame too short: len=%u (need >= 9)", static_cast<unsigned>(msgElement.length));
                ++statsBad;
                continue; // Skip processing for this frame
            }
            uint32_t sensor_time_ms =
                (static_cast<uint32_t>(msgElement.data[0]) << 24) |
                (static_cast<uint32_t>(msgElement.data[1]) << 16) |
                (static_cast<uint32_t>(msgElement.data[2]) << 8) |
                static_cast<uint32_t>(msgElement.data[3]);
            const uint8_t frameSource = msgElement.data[4] >> 4;  // 0 chest, 1 watch; Local never travels over BLE
            const uint8_t msgType = msgElement.data[4] & 0x0F;
            uint32_t sequence =
                (static_cast<uint32_t>(msgElement.data[5]) << 24) |
                (static_cast<uint32_t>(msgElement.data[6]) << 16) |
                (static_cast<uint32_t>(msgElement.data[7]) << 8) |
                static_cast<uint32_t>(msgElement.data[8]);
            LOGT(TAG, "frame msgType=%u sequence=%lu sensor_ms=%lu len=%u", static_cast<unsigned>(msgType),
                 static_cast<unsigned long>(sequence), static_cast<unsigned long>(sensor_time_ms),
                 static_cast<unsigned>(msgElement.length));

            if (frameSource > static_cast<uint8_t>(SensorSource::Watch)) {
                LOGW(TAG, "invalid frame source=%u dropped", static_cast<unsigned>(frameSource));
                ++statsBad;
                continue;
            }
            // 只收與目前選擇相同 source 的 PPG / IMU frame；選的是 Local 時 BLE 端全丟。
            if ((msgType == 0x01 && frameSource != static_cast<uint8_t>(activePpgSource)) ||
                (msgType >= 0x02 && msgType <= 0x04 && frameSource != static_cast<uint8_t>(activeImuSource))) {
                continue;
            }

            if (msgType == 0x01) { // PPG sensor
                if (msgElement.length < 20) {
                    LOGW(TAG, "PPG frame too short: len=%u (need >= 20)", static_cast<unsigned>(msgElement.length));
                    ++statsBad;
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
                ++statsPpg;
                LOGD(TAG, "PPG sensor_ms=%lu sequence=%lu status=%u SpO2=%u HR=%u bpi=%u hrv=%u amp=%u systole=%u "
                          "ppg1=%u ppg2=%u",
                     static_cast<unsigned long>(sensor_time_ms), static_cast<unsigned long>(sequence),
                     static_cast<unsigned>(ppgElement.status), static_cast<unsigned>(ppgElement.SpO2),
                     static_cast<unsigned>(ppgElement.heartRateBpm), static_cast<unsigned>(ppgElement.bpi),
                     static_cast<unsigned>(ppgElement.hrv), static_cast<unsigned>(ppgElement.pulseAmplitude),
                     static_cast<unsigned>(ppgElement.systoleDuration), static_cast<unsigned>(ppgElement.ppg1),
                     static_cast<unsigned>(ppgElement.ppg2));
                if (xQueueSend(ppgSensorQueue, &ppgElement, 0) != pdPASS) {
                    LOGW(TAG, "PPG queue full, sample dropped (sequence=%lu)",
                         static_cast<unsigned long>(ppgElement.sequence));
                }
                UiUpdateElement ppgUpdate{};
                ppgUpdate.type = UiUpdateType::Sensor;
                ppgUpdate.timestampMs = ppgElement.hub_rx_time_ms;
                ppgUpdate.heartRateBpm = ppgElement.heartRateBpm;
                ppgUpdate.spo2 = ppgElement.SpO2;
                ppgUpdate.sensorStatus = ppgElement.status;
                postUi(ppgUpdate);
            } else if (msgType == 0x02) { // acc + gyro in one frame
                if (msgElement.length < 21) {
                    LOGW(TAG, "IMU frame too short: len=%u (need >= 21)", static_cast<unsigned>(msgElement.length));
                    ++statsBad;
                    continue; // Skip processing for this frame
                }
                handleAcc(msgElement, sensor_time_ms, sequence, &msgElement.data[9], statsAcc);
                handleGyro(msgElement, sensor_time_ms, sequence, &msgElement.data[15], statsGyro);
            } else if (msgType == 0x03) { // Acc only
                if (msgElement.length < 15) {
                    LOGW(TAG, "Acc frame too short: len=%u (need >= 15)", static_cast<unsigned>(msgElement.length));
                    ++statsBad;
                    continue; // Skip processing for this frame
                }
                handleAcc(msgElement, sensor_time_ms, sequence, &msgElement.data[9], statsAcc);
            } else if (msgType == 0x04) { // Gyro only
                if (msgElement.length < 15) {
                    LOGW(TAG, "Gyro frame too short: len=%u (need >= 15)", static_cast<unsigned>(msgElement.length));
                    ++statsBad;
                    continue; // Skip processing for this frame
                }
                handleGyro(msgElement, sensor_time_ms, sequence, &msgElement.data[9], statsGyro);
            } else {
                LOGW(TAG, "unknown msgType=0x%02X (len=%u, sequence=%lu)", static_cast<unsigned>(msgType),
                     static_cast<unsigned>(msgElement.length), static_cast<unsigned long>(sequence));
                ++statsBad;
                continue; // Skip processing for unknown msgType
            }
        }
		vTaskDelay(pdMS_TO_TICKS(100));
	}
}