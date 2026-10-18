#include "BLETask.h"

void scanCB(T_LE_CB_DATA* p_data); // defined below, outside the anonymous namespace

namespace {

constexpr const char* TAG = "BLE";
constexpr uint32_t BLE_AUTO_SCAN_MS = 2000;
constexpr uint32_t BLE_MANUAL_SCAN_MS = 3000;
constexpr uint8_t BLE_MANUAL_TRIES = 3;
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

// One link per sensor, index = SensorSource (Chest, Watch). The frames themselves carry the
// source in the msgType high nibble, so the link only decides whom to connect to.
// Auto retries every BLE_RETRY_DELAY_MS until connected and turns into Manual when the link
// drops; Manual scans only after a Connect command (BLE_MANUAL_TRIES attempts).
struct Link {
    const char* label;
    const char* name;      // advertised name
    bool autoMode;
    BLEAdvertData adv;     // written by scanCB
    volatile bool found;
    int8_t connId;
    BLEClient* client;     // deleted by the BLE library on disconnect
    bool up;
    uint8_t manualTries;
    uint32_t lastAttemptMs;
    uint32_t attempts;
};
constexpr uint8_t LINK_COUNT = 2;
Link links[LINK_COUNT] = {{"chest", CHEST_NAME, true}, {"watch", WATCH_NAME, true}};
Link* volatile scanTarget = nullptr;  // the link scanCB is looking for
BLEAdvertData foundDevice;
bool clientConfigured = false;

// Written by notificationCB, read by BLETask for the stats line.
volatile uint32_t cbFrames = 0;
volatile uint32_t cbQueueFull = 0;
volatile uint32_t cbOversize = 0;
uint32_t statsPpg = 0, statsAcc = 0, statsGyro = 0, statsBad = 0;

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

bool linkAlive(const Link& l)
{
    return l.connId >= 0 && BLE.connected(l.connId);
}

void dropLink(Link& l, const char* reason)
{
    LOGW(TAG, "%s: dropping link: %s", l.label, reason);
    if (linkAlive(l)) {
        BLE.configConnection()->disconnect(l.connId);
    }
    l.connId = -1;
    l.client = nullptr;
    l.up = false;
}

void postLinks(int8_t connecting = -1)
{
    UiUpdateElement update{};
    update.type = UiUpdateType::Ble;
    update.timestampMs = millis();
    for (uint8_t i = 0; i < LINK_COUNT; ++i) {
        const BleLinkState state = links[i].up ? BleLinkState::Up
                                   : i == connecting ? BleLinkState::Connecting : BleLinkState::Down;
        update.bleLink[i] = static_cast<uint8_t>(state);
        update.bleAuto[i] = links[i].autoMode ? 1 : 0;
    }
    postUi(update);
}

void processFrame(const BleFrameElement& msg)
{
        if (msg.length >= 5 && (msg.data[4] & 0x0F) == 0x00) { // cmd: reserved, payload not defined yet
            LOGD(TAG, "cmd frame ignored (len=%u)", static_cast<unsigned>(msg.length));
            return;
        }
        if (msg.length < 9) {
            LOGW(TAG, "frame too short: len=%u (need >= 9)", static_cast<unsigned>(msg.length));
            ++statsBad;
            return; // Skip processing for this frame
        }
        uint32_t sensor_time_ms =
            (static_cast<uint32_t>(msg.data[0]) << 24) |
            (static_cast<uint32_t>(msg.data[1]) << 16) |
            (static_cast<uint32_t>(msg.data[2]) << 8) |
            static_cast<uint32_t>(msg.data[3]);
        const uint8_t frameSource = msg.data[4] >> 4;  // 0 chest, 1 watch; Local never travels over BLE
        const uint8_t msgType = msg.data[4] & 0x0F;
        uint32_t sequence =
            (static_cast<uint32_t>(msg.data[5]) << 24) |
            (static_cast<uint32_t>(msg.data[6]) << 16) |
            (static_cast<uint32_t>(msg.data[7]) << 8) |
            static_cast<uint32_t>(msg.data[8]);
        LOGT(TAG, "frame msgType=%u sequence=%lu sensor_ms=%lu len=%u", static_cast<unsigned>(msgType),
             static_cast<unsigned long>(sequence), static_cast<unsigned long>(sensor_time_ms),
             static_cast<unsigned>(msg.length));

        if (frameSource > static_cast<uint8_t>(SensorSource::Watch)) {
            LOGW(TAG, "invalid frame source=%u dropped", static_cast<unsigned>(frameSource));
            ++statsBad;
            return;
        }
        // 只收與目前選擇相同 source 的 PPG / IMU frame；選的是 Local 時 BLE 端全丟。
        if ((msgType == 0x01 && frameSource != static_cast<uint8_t>(activePpgSource)) ||
            (msgType >= 0x02 && msgType <= 0x04 && frameSource != static_cast<uint8_t>(activeImuSource))) {
            return;
        }

        if (msgType == 0x01) { // PPG sensor
            if (msg.length < 20) {
                LOGW(TAG, "PPG frame too short: len=%u (need >= 20)", static_cast<unsigned>(msg.length));
                ++statsBad;
                return; // Skip processing for this frame
            }
            PpgSensorElement ppgElement;
            ppgElement.hub_rx_time_ms = msg.hub_rx_time_ms;
            ppgElement.sensor_time_ms = sensor_time_ms;
            ppgElement.sequence = sequence;
            ppgElement.status = msg.data[9];
            ppgElement.SpO2 = msg.data[10];
            ppgElement.heartRateBpm = msg.data[11];
            ppgElement.bpi = msg.data[12];
            ppgElement.hrv = msg.data[13];
            ppgElement.pulseAmplitude = msg.data[14];
            ppgElement.systoleDuration = msg.data[15];
            ppgElement.ppg1 = (msg.data[16] << 8) | msg.data[17];
            ppgElement.ppg2 = (msg.data[18] << 8) | msg.data[19];
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
            if (msg.length < 21) {
                LOGW(TAG, "IMU frame too short: len=%u (need >= 21)", static_cast<unsigned>(msg.length));
                ++statsBad;
                return; // Skip processing for this frame
            }
            handleAcc(msg, sensor_time_ms, sequence, &msg.data[9], statsAcc);
            handleGyro(msg, sensor_time_ms, sequence, &msg.data[15], statsGyro);
        } else if (msgType == 0x03) { // Acc only
            if (msg.length < 15) {
                LOGW(TAG, "Acc frame too short: len=%u (need >= 15)", static_cast<unsigned>(msg.length));
                ++statsBad;
                return; // Skip processing for this frame
            }
            handleAcc(msg, sensor_time_ms, sequence, &msg.data[9], statsAcc);
        } else if (msgType == 0x04) { // Gyro only
            if (msg.length < 15) {
                LOGW(TAG, "Gyro frame too short: len=%u (need >= 15)", static_cast<unsigned>(msg.length));
                ++statsBad;
                return; // Skip processing for this frame
            }
            handleGyro(msg, sensor_time_ms, sequence, &msg.data[9], statsGyro);
        } else {
            LOGW(TAG, "unknown msgType=0x%02X (len=%u, sequence=%lu)", static_cast<unsigned>(msgType),
                 static_cast<unsigned>(msg.length), static_cast<unsigned long>(sequence));
            ++statsBad;
            return; // Skip processing for unknown msgType
        }
}

// Drains received frames; also called while a connect attempt waits, so the other link's
// data keeps flowing during a scan or service discovery.
void pumpFrames()
{
    BleFrameElement msg;
    while (xQueueReceive(bleRxFrameQueue, &msg, 0) == pdPASS) processFrame(msg);
}

void waitMs(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
    pumpFrames();
}

bool connectLink(Link& l, uint32_t scanMs)
{
    l.found = false;
    l.connId = -1;
    l.client = nullptr;
    ++l.attempts;
    LOGI(TAG, "%s attempt #%lu: scanning for '%s' (%lu ms)", l.label, static_cast<unsigned long>(l.attempts),
         l.name, static_cast<unsigned long>(scanMs));
    // startScan(ms) always waits the full duration; stop as soon as the target is seen.
    scanTarget = &l;
    BLE.configScan()->startScan();
    for (uint32_t waited = 0; waited < scanMs && !l.found && !systemStandby; waited += 20) {
        waitMs(20);
    }
    BLE.configScan()->stopScan();
    scanTarget = nullptr;
    waitMs(100);  // same settle time the library's timed scan leaves
    if (systemStandby) {
        LOGI(TAG, "%s: scan aborted: standby", l.label);
        return false;
    }
    if (!l.found) {
        LOGW(TAG, "%s: '%s' not found", l.label, l.name);
        return false;
    }

    LOGI(TAG, "%s: connecting to %s (timeout %lu ms)", l.label, l.adv.getAddr().str(),
         static_cast<unsigned long>(BLE_CONNECT_TIMEOUT_MS));
    if (!BLE.configConnection()->connect(l.adv, BLE_CONNECT_TIMEOUT_MS)) {
        LOGW(TAG, "%s: connect request failed", l.label);
        return false;
    }
    const uint32_t connectStart = millis();
    while ((uint32_t)(millis() - connectStart) < BLE_CONNECT_TIMEOUT_MS) {
        l.connId = BLE.configConnection()->getConnId(l.adv);
        if (linkAlive(l)) break;
        l.connId = -1;
        waitMs(100);
    }
    if (l.connId < 0) {
        LOGW(TAG, "%s: connect timeout after %lu ms", l.label, static_cast<unsigned long>(BLE_CONNECT_TIMEOUT_MS));
        return false;
    }
    LOGI(TAG, "%s: connected, connID=%d (%lu ms)", l.label, static_cast<int>(l.connId),
         static_cast<unsigned long>(millis() - connectStart));

    if (!clientConfigured) {
        BLE.configClient();
        clientConfigured = true;
    }
    // Needs the patched library in external/libraries/BLE (registers the GATT client once).
    l.client = BLE.addClient(l.connId);
    if (l.client == nullptr) {
        dropLink(l, "add client failed");
        return false;
    }
    l.client->discoverServices();
    LOGI(TAG, "%s: discovering services (timeout %lu ms)", l.label,
         static_cast<unsigned long>(BLE_DISCOVERY_TIMEOUT_MS));
    const uint32_t discoveryStart = millis();
    while (!l.client->discoveryDone()) {
        if (!linkAlive(l)) {
            dropLink(l, "disconnected during discovery");
            return false;
        }
        if ((uint32_t)(millis() - discoveryStart) >= BLE_DISCOVERY_TIMEOUT_MS) {
            dropLink(l, "service discovery timeout");
            return false;
        }
        waitMs(100);
    }
    LOGI(TAG, "%s: service discovery done in %lu ms", l.label,
         static_cast<unsigned long>(millis() - discoveryStart));

    BLERemoteService* service = nullptr;
    BLERemoteCharacteristic* tx = nullptr;
    const UartUuidSet* uuids = nullptr;
    for (const UartUuidSet& set : UART_UUID_SETS) {
        service = l.client->getService(set.service);
        if (service == nullptr) continue;
        tx = service->getCharacteristic(set.tx);
        if (tx != nullptr) {
            uuids = &set;
            break;
        }
        LOGW(TAG, "%s: TX characteristic %s not found in service %s", l.label, set.tx, set.service);
    }
    if (uuids == nullptr) {
        dropLink(l, "UART service/TX characteristic not found");
        return false;
    }
    LOGI(TAG, "%s: using service %s (TX=%s)", l.label, uuids->service, uuids->tx);
    tx->setBufferLen(STRING_BUF_SIZE);
    tx->setNotifyCallback(notificationCB);
    tx->enableNotifyIndicate();
    l.up = true;
    LOGI(TAG, "%s: connected, notifications enabled", l.label);
    return true;
}

void applyCommand(const BleCmdElement& cmd)
{
    const uint8_t index = static_cast<uint8_t>(cmd.device);
    if (index >= LINK_COUNT) {
        LOGW(TAG, "command %u for invalid device %u ignored", static_cast<unsigned>(cmd.command),
             static_cast<unsigned>(index));
        return;
    }
    Link& l = links[index];
    switch (cmd.command) {
    case BleCommandType::Connect:
        LOGI(TAG, "%s: connect requested", l.label);
        if (!l.up) l.manualTries = BLE_MANUAL_TRIES;
        break;
    case BleCommandType::Disconnect:
        LOGI(TAG, "%s: disconnect requested, mode -> manual", l.label);
        l.autoMode = false;
        l.manualTries = 0;
        if (l.up) dropLink(l, "user request");
        break;
    case BleCommandType::SetAuto:
        LOGI(TAG, "%s: mode -> auto", l.label);
        l.autoMode = true;
        l.lastAttemptMs = millis() - BLE_RETRY_DELAY_MS;  // first attempt right away
        break;
    case BleCommandType::SetManual:
        LOGI(TAG, "%s: mode -> manual", l.label);
        l.autoMode = false;
        break;
    }
}

} // namespace

void scanCB(T_LE_CB_DATA* p_data)
{
    Link* target = scanTarget;
    if (target == nullptr || target->found) return;
    foundDevice.parseScanInfo(p_data);
    if (foundDevice.hasName() && foundDevice.getName() == String(target->name)) {
        LOGI("BLE", "scan: found %s at %s", target->label, foundDevice.getAddr().str());
        target->adv = foundDevice;
        target->found = true;
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
    (void)parameter;
    BLE.init();
    BLE.setScanCallback(scanCB);
    BLE.beginCentral(LINK_COUNT);
    LOGI(TAG, "BLE stack initialized (central, %u connections)", static_cast<unsigned>(LINK_COUNT));

    for (Link& l : links) {
        l.connId = -1;
        l.lastAttemptMs = millis() - BLE_RETRY_DELAY_MS;  // Auto links try right away
    }
    uint32_t lastStatsMs = millis();
    uint32_t statsFrames = 0, statsQueueFull = 0;
    uint32_t lastUiBleMs = 0;
    for (;;) {
        BleCmdElement cmd;
        while (xQueueReceive(bleCmdQueue, &cmd, 0) == pdPASS) {
            applyCommand(cmd);
            lastUiBleMs = millis() - 1000u;  // show the new mode at once
        }

        for (uint8_t i = 0; i < LINK_COUNT; ++i) {
            Link& l = links[i];
            // Standby: drop the links and stop scanning; the modes stay, so Auto links reconnect on wake.
            if (systemStandby) {
                if (l.up) dropLink(l, "standby");
                l.manualTries = 0;
                l.lastAttemptMs = millis() - BLE_RETRY_DELAY_MS;
                continue;
            }
            if (l.up && !linkAlive(l)) {
                dropLink(l, "disconnected");
                if (l.autoMode) {
                    l.autoMode = false;
                    LOGW(TAG, "%s: link lost, mode -> manual", l.label);
                }
                lastUiBleMs = millis() - 1000u;
            }
            if (l.up) continue;
            const bool manual = l.manualTries > 0;
            if (!manual && !(l.autoMode && (uint32_t)(millis() - l.lastAttemptMs) >= BLE_RETRY_DELAY_MS)) {
                continue;
            }
            if (manual) --l.manualTries;
            postLinks(static_cast<int8_t>(i));
            if (connectLink(l, manual ? BLE_MANUAL_SCAN_MS : BLE_AUTO_SCAN_MS)) {
                l.manualTries = 0;
            } else if (manual) {
                LOGW(TAG, "%s: manual connect failed, %u tries left", l.label, static_cast<unsigned>(l.manualTries));
            }
            l.lastAttemptMs = millis();
            lastUiBleMs = millis() - 1000u;
        }

        // Resent every second so a dropped update cannot leave a stale link state on screen.
        if ((uint32_t)(millis() - lastUiBleMs) >= 1000u) {
            postLinks();
            lastUiBleMs = millis();
        }

        if ((uint32_t)(millis() - lastStatsMs) >= 5000u) {
            const uint32_t frames = cbFrames, full = cbQueueFull;
            LOGD(TAG, "5s stats: chest=%s watch=%s rx=%lu (queue full %lu) PPG=%lu Acc=%lu Gyro=%lu bad=%lu, "
                      "oversize=%lu",
                 links[0].up ? "up" : "down", links[1].up ? "up" : "down",
                 static_cast<unsigned long>(frames - statsFrames),
                 static_cast<unsigned long>(full - statsQueueFull),
                 static_cast<unsigned long>(statsPpg), static_cast<unsigned long>(statsAcc),
                 static_cast<unsigned long>(statsGyro), static_cast<unsigned long>(statsBad),
                 static_cast<unsigned long>(cbOversize));
            statsFrames = frames;
            statsQueueFull = full;
            statsPpg = statsAcc = statsGyro = statsBad = 0;
            lastStatsMs = millis();
        }

        waitMs(20);
    }
}
