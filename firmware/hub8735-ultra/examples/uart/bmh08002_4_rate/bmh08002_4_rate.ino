// Count BMH08002-4 packets on UART3 inside the HUB8735 (no PC bridge in the data path).
// Prints once per second: bytes, good packets, bad packets, resyncs, peak RX buffer fill,
// and the min/avg/max gap (us) between packet starts. A steady ~20000 gap with fewer packets
// than expected means loss; a steady ~28000 gap means the module itself sends that slowly.
// Compare "good/s" with what bmh08002_control.py measure reports to see where packets are lost.

#define MODULE_BAUD 38400
#define PACKET_LEN 15

static const uint8_t START_MEASURE[8] = {0x55, 0xB1, 0x01, 0x00, 0x00, 0x00, 0xB2, 0xAA};

static uint8_t pkt[PACKET_LEN];
static uint8_t pos = 0;
static uint32_t bytesRx, good, bad, resync, maxFill;
static uint32_t lastReport;
static uint32_t lastStartUs, gapMin = 0xFFFFFFFF, gapMax, gapSum, gapCnt;

static void feed(uint8_t b)
{
    bytesRx++;
    if (pos == 0 && b != 0x55) { resync++; return; }
    if (pos == 1 && b != 0xB0) { resync++; pos = (b == 0x55) ? 1 : 0; if (pos) pkt[0] = b; return; }
    if (pos == 0) {
        uint32_t us = micros();
        if (lastStartUs) {
            uint32_t gap = us - lastStartUs;
            if (gap < gapMin) gapMin = gap;
            if (gap > gapMax) gapMax = gap;
            gapSum += gap;
            gapCnt++;
        }
        lastStartUs = us;
    }
    pkt[pos++] = b;
    if (pos < PACKET_LEN) return;
    pos = 0;
    uint8_t sum = 0;
    for (int i = 1; i < 13; i++) sum += pkt[i];
    if (pkt[14] == 0xAA && sum == pkt[13]) good++; else bad++;
}

void setup()
{
    Serial.begin(115200);
    Serial3.begin(MODULE_BAUD);
    Serial3.write(START_MEASURE, sizeof(START_MEASURE));
    lastReport = millis();
}

void loop()
{
    uint32_t fill = Serial3.available();
    if (fill > maxFill) maxFill = fill;
    while (Serial3.available()) feed(Serial3.read());

    uint32_t now = millis();
    if (now - lastReport >= 1000) {
        Serial.print("bytes=");   Serial.print(bytesRx);
        Serial.print(" good=");   Serial.print(good);
        Serial.print(" bad=");    Serial.print(bad);
        Serial.print(" resync="); Serial.print(resync);
        Serial.print(" maxFill=");Serial.print(maxFill);
        Serial.print(" gap_us min/avg/max=");
        Serial.print(gapCnt ? gapMin : 0); Serial.print("/");
        Serial.print(gapCnt ? gapSum / gapCnt : 0); Serial.print("/");
        Serial.println(gapMax);
        bytesRx = good = bad = resync = maxFill = 0;
        gapMin = 0xFFFFFFFF; gapMax = gapSum = gapCnt = 0;
        lastReport += 1000;
    }
}
