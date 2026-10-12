/*
 HUB8735 作為 BLE Peripheral（GATT server）：
 對端開啟 notify（寫 CCCD）後，每秒送一包 25 byte 測試封包（內容 0x00~0x18）。

 注意：25 byte 超過預設 MTU 23 的 20 byte 有效載荷，
 對端（central）需先協商 MTU >= 28，否則封包會被截斷或送出失敗。
*/

#include "BLEDevice.h"

#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NOTIFY_UUID  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

#define PACKET_LEN  25
#define SEND_PERIOD 1000

BLEService service(SERVICE_UUID);
BLECharacteristic notifyChr(NOTIFY_UUID);
BLEAdvertData advdata;
BLEAdvertData scndata;

volatile bool notifyEnabled = false;

void cccdCB(BLECharacteristic* chr, uint8_t connID, uint16_t cccd)
{
    notifyEnabled = (cccd & GATT_CLIENT_CHAR_CONFIG_NOTIFY) != 0;
    printf("Notify %s on connection %d\n", notifyEnabled ? "enabled" : "disabled", connID);
}

void setup()
{
    Serial.begin(115200);

    advdata.addFlags(GAP_ADTYPE_FLAGS_LIMITED | GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED);
    advdata.addCompleteName("HUB8735_NOTIFY");
    scndata.addCompleteServices(BLEUUID(SERVICE_UUID));

    notifyChr.setReadProperty(true);
    notifyChr.setReadPermissions(GATT_PERM_READ);
    notifyChr.setNotifyProperty(true);
    notifyChr.setCCCDCallback(cccdCB);
    notifyChr.setBufferLen(PACKET_LEN);

    service.addCharacteristic(notifyChr);

    BLE.init();
    BLE.configAdvert()->setAdvData(advdata);
    BLE.configAdvert()->setScanRspData(scndata);
    BLE.configServer(1);
    BLE.addService(service);
    BLE.beginPeripheral();
}

void loop()
{
    static uint32_t lastSend = 0;

    if (notifyEnabled && BLE.connected(0) && millis() - lastSend >= SEND_PERIOD) {
        lastSend = millis();

        uint8_t packet[PACKET_LEN];
        for (uint8_t i = 0; i < PACKET_LEN; i++) {
            packet[i] = i;
        }
        notifyChr.setData(packet, PACKET_LEN);
        notifyChr.notify(0);
        Serial.println("Sent 25-byte notify");
    }
    delay(10);
}
